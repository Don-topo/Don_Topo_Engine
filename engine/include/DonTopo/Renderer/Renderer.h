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
#include "DonTopo/Renderer/InstanceBuffers.h"
#include "DonTopo/Renderer/VisibleSet.h"
#include "DonTopo/Renderer/RendererState.h"
#include "DonTopo/Renderer/GpuDevice.h"
#include "DonTopo/Renderer/GpuResources.h"
#include "DonTopo/Renderer/SharedGpuMesh.h"
#include "DonTopo/Renderer/SharedTextureCache.h"
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
#include "DonTopo/Renderer/Passes/SelectionOutlinePass.h"
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

    // The get/set of the scalar quality and effects state (ambient, bloom,
    // SSAO, SSR, fog, FXAA, TAA, Forward+) are provided by RendererState: they do not
    // depend on Vulkan and are shared with the DirectX 12 backend.
    class Renderer : public EditorRenderer {
        // --- Editing API (consumed only by DonTopoEditor) ---
        //
        // After the Core/Editor split the boundary is a build one (two targets), not
        // an API one: these public methods are still reachable from any
        // point in Core even though they were designed for the editor. They are listed here
        // because a future extension point would come out of this list, and because
        // the next reader needs to know they are NOT for the runtime:
        //
        //   setUiLayer                        injection of the ImGui layer
        //   setViewportSize / viewportAspect  the render goes to a panel, not to the window
        //   setOutlineTarget                  selection outline
        //   removeMeshComponent               delete an asset in use from the Content Browser
        //   replaceStaticTextureWithMissing   same, texture -> placeholder
        //   rebuildSkinnedMesh                reimport an FBX with the editor open
        //   requestProbeBake / ...All         probe baking (authoring tool)
        //   setPerfCaptureEnabled             PerformancePanel capture
        //
        // What does NOT go in, even though today only the editor calls it: the graphics
        // quality get/set (bloom, fog, ssao, ssr, taa, msaa, ssaa, fxaa,
        // forward+) and the stats counters. An exported game has
        // legitimate reasons to touch them from its options menu; capping there
        // would be closing the door on the use case, not tidying the boundary.
        //
        // Careful with two that the project memory took as editor-only and are not:
        // registerGameObject and flushUploadsAndWait are also called by
        // runtime/main.cpp.
        public:
            Renderer()                              = default;
            ~Renderer();
            Renderer(const Renderer&)               = delete;
            Renderer& operator=(const Renderer&)    = delete;
            void init(Window& window, const std::vector<Mesh>& meshes);
            // Phase 1 of startup: the minimum to present (device, swapchain,
            // render pass, framebuffers, command buffers, sync, UI layer). It does not create
            // scene pipelines. Used by the runtime to be able to draw the
            // splash before the heavy load. init() calls it first.
            void initPresentation(Window& window);
            // Phase 2: PBR/shadow/compute pipelines, offscreen, descriptor sets,
            // upload of static meshes and camera auto-fit (needs meshes).
            // init() calls it afterwards.
            void initSceneResources(const std::vector<Mesh>& meshes);
            // Initializes the splash on the swapchain's render pass (requires
            // initPresentation already called). false if the logo does not load: the caller
            // skips the splash. Only the runtime calls it; the editor does not.
            bool beginSplash(const std::string& logoPath);
            // Presents a frame with only the splash at alpha [0,1]. No-op if the
            // splash was not initialized.
            void drawSplashFrame(float alpha);
            void drawFrame(Window& window);
            // Once per frame, from the main loop, BEFORE drawFrame.
            // It is what advances the deferred destruction queue.
            void tickDeferredDeletes();
            void shutdown();
            void setCamera(const Camera& camera);
            void notifyResize() { m_framebufferResized = true; }
            // Selection outline: indices of the highlighted object in m_objects and
            // in m_skinnedObjects (the same ones GameObject keeps in
            // staticRenderIndex/skinnedRenderIndex), -1 for "none". Only the
            // editor calls it, once per frame; the runtime has no selection
            // and with the default (-1, -1) no outline is drawn. The highlighted
            // object is still subject to the same culling as the rest: outside the
            // frustum nothing is drawn.
            void setOutlineTarget(int staticIndex, int skinnedIndex)
            {
                m_outlinePass.setTarget(staticIndex, skinnedIndex);
            }
            // In headless there is no editor to press Play: the runtime starts
            // playing from frame 0. This is also what makes
            // currentFrameCamera() pick the scene's CameraComponent instead
            // of the editor's fly camera.
            bool isPlaying() const;
            // Runtime mode: no UI and no panels. It only has an effect if it is
            // called BEFORE initPresentation() (or init(), which calls it):
            // createOffscreenImages and the UI startup read the flag
            // during that initialization.
            void setHeadless(bool headless) { m_headless = headless; }
            // UI layer, non-owning: it is the editor that owns the Renderer
            // and registers here. nullptr (runtime) = there is no UI pass.
            // It must be set BEFORE initPresentation(), which is what
            // starts it.
            void setUiLayer(UiLayer* ui) { m_ui = ui; }

            // Canvas of the GAME UI (not the editor), the SCREEN one. It is drawn
            // inside the composition pass, on top of the scene and below
            // ImGui, and when empty it costs not a single command. Returning the reference acts
            // as getter and setter: the tree is built on uiCanvas().root().
            //
            // With N canvases (Task 5 of the world canvas) this is no longer a field:
            // it looks for the first screen slot among m_uiSlots. With none (the
            // scene only has world canvases, or none) it falls back to
            // m_uiCanvasFallback, which is persistent and empty: returning the
            // reference to a temporary would leave the gizmos reading dead
            // memory.
            UiCanvas&       uiCanvas() override;
            const UiCanvas& uiCanvas() const;

            // ALL the screen ones, in input priority order (the topmost
            // first). It is what divides the mouse among several canvases.
            void screenUiCanvases(std::vector<UiCanvas*>& out) override;

            // The canvas of a GameObject by its id (nullptr if it has none). Used by
            // the gizmo of the selected canvas, which cannot rely on uiCanvas().
            const UiCanvas* uiCanvasOf(uint64_t ownerId) const override;

            // Builds the live tree of EACH canvas in the scene, one per
            // CanvasComponent. Replaces the collect + syncUiWidgets that the three
            // loops (runtime and sandbox x2) used to repeat.
            void syncUiCanvases(const std::vector<UiCanvasBinding>& bindings) override;

            // Looks up a node by name in ALL the canvases, not just the screen
            // one. Needed by the editor's widget gizmos: without this,
            // a button inside a world canvas would be left without a gizmo.
            const UiElement* findUiNode(const std::string& name) const override;
            // Loads an atlas from disk and reserves its descriptor set. The
            // Renderer is the owner; it returns nullptr if the image cannot be
            // read. Sprites are added later with addSprite.
            UiTextureAtlas* loadUiAtlas(const std::string& path);
            // Same as loadUiAtlas but for a TTF: it bakes the MSDF atlas, reserves
            // its descriptor set and returns nullptr if the file cannot be
            // read. The Renderer is the owner of the font. It does not cache
            // by path either: whoever asks repeatedly (the button sync)
            // keeps its own cache, same as with the atlases.
            UiFont* loadUiFont(const std::string& path, float bakePx = 48.0f);
            // currentFrameCamera() needs to ask the scene for its
            // camera every frame (Scene::findCamera is the single source
            // of truth).
            void setScene(Scene* scene) { m_scene = scene; }

            // Aspect of the render target. Public because the frustum gizmo
            // (ViewportPanel) has to use EXACTLY the same one the Play
            // projection will use, or it would draw a framing that does not match.
            float viewportAspect() const
            {
                const VkExtent2D e = effectiveViewport();
                return e.height > 0 ? (float)e.width / (float)e.height : 1.0f;
            }
            // Size in pixels of the INTERNAL render target. With SSAA it is larger
            // than the output one and it is NOT the UI canvas space: for that
            // there are uiWidth/uiHeight.
            uint32_t renderWidth()  const { return m_renderExtent.width; }
            uint32_t renderHeight() const { return m_renderExtent.height; }
            // OUTPUT size in pixels, the same one passed to
            // UiCanvas::buildDrawData and in which the mouse has to be fed to
            // UiCanvas::updateInput. Public because the canvas is resolved in it
            // and whoever passes it the mouse (runtime and editor) has to use it.
            uint32_t uiWidth()  const { return effectiveViewport().width; }
            uint32_t uiHeight() const { return effectiveViewport().height; }
            uint64_t uiAtlasTextureId(const UiTextureAtlas* atlas) override;
            uint64_t uiThumbnailAtlasId() override;
            bool     uploadUiThumbnails(const ThumbnailTile* tiles, size_t count) override;
            // EXACT size of the image area of the editor's Viewport panel, in
            // pixels. The editor calls it once per frame. Without this the render
            // would go to the WINDOW size and the panel would rescale it when drawing:
            // that bilinear rescaling smooths the edges on its own (it eats
            // the stair-stepping and with it the difference between anti-aliasing modes) and
            // also deforms the image when the panel's aspect does not match
            // the window's. Changing it recreates the targets, like a resize.
            // The runtime never calls it: there the target is the whole swapchain.
            void setViewportSize(uint32_t width, uint32_t height);
            // Recomputes the scene's reference framing (center and size) from
            // what is in the GPU NOW, not from the meshes that init() received.
            // It matters because m_cameraDistance yields the near and the far of
            // the editor's projection (near = d*0.001, far = d*3): without this a
            // scene loaded after startup is drawn with the range of another one
            // (the startup one), and whatever falls beyond that far (the skybox first)
            // is clipped. The editor calls it when loading a scene and when the
            // async assets land; the runtime never calls it. If there is nothing
            // to bound (empty scene) it keeps the current range.
            void refitCameraRange();
            void setSceneRoot(GameObject* root) { m_sceneRoot = root; }
            // Frees mesh/skinnedMesh/textures on the GPU of node and its whole subtree
            // (called by EditorUI right before deleting the node from the scene graph).
            void removeGameObject(GameObject* node);
            // Inverse of removeGameObject: uploads to the GPU the meshes (static or
            // skinned) of node and its subtree that are not yet registered
            // (staticRenderIndex/skinnedRenderIndex < 0). Used after
            // rebuilding a subtree from JSON: reloadSceneFromJson (the whole
            // scene) and CreateGameObjectCommand/DeleteGameObjectCommand in
            // Command.cpp (a single subtree).
            void registerGameObject(GameObject* node);
            // Removes only the Mesh component of go (it does not delete the GameObject nor its
            // other components). No-op if go is nullptr or has no mesh.
            void removeMeshComponent(GameObject* go);
            // Replaces the texture of the indicated slot with the "missing"
            // checkerboard (same generator that createTextureImage uses when there is
            // no path/bytes). No-op if renderIndex is out of range.
            // It waits for the GPU before rewriting the descriptor set: doing it
            // with a command buffer in flight that has it bound is invalid
            // use without UPDATE_AFTER_BIND, and these sets do not request it (H25).
            // The old RESOURCES, on the other hand, go to the deferred destruction
            // queue and do not depend on that wait. It only covers static meshes
            // (there is no UI today that assigns skinned meshes).
            void replaceStaticTextureWithMissing(int renderIndex, TextureSlot slot);
            // facePaths: +X, -X, +Y, -Y, +Z, -Z (any format supported by stb_image)
            void initSkybox(const std::array<std::string, 6>& facePaths);
            void setTransform(size_t objectIndex, const glm::mat4& transform)
            {
                if (objectIndex < m_objects.size())
                    m_objects[objectIndex].transform = transform;
            }
            // SSR strength of the object (0 = does not reflect). It travels to the post-pass
            // through the alpha of the HDR attachment, which pbr.frag writes from
            // PushData::flags.y. It is synchronized per frame along with the transform, same
            // as the runtime does: this way Play Mode, Undo and scene loading
            // need no path of their own.
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
            // See EditorRenderer::setObjectMaterialFactors. It goes through
            // setEffectiveFactors, which is where the rule "with an ORM map
            // the map rules" lives: without that, dragging the slider over an object with a
            // map would put the slider value right where registration sets
            // 1.0. Out of line because it needs to look at the shared entry.
            void setObjectMaterialFactors(size_t objectIndex, float metallic, float roughness);
            // Visibility of the mesh (false = not drawn). It is synchronized per frame
            // along with the transform, same as SSR, so Play Mode, Undo and
            // scene loading need no path of their own. It is consumed by the
            // scene, shadow and AO passes: hidden, it is not sent to the GPU in any of them.
            // Physics, selection and outline do not look at it.
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
            // All of this lives in ReflectionProbePass; here it is only delegated so as not to
            // move the API the editor sees. The UI only QUEUES: the bake happens
            // at the start of drawFrame, which is where you can wait for the
            // GPU to be free without catching the command buffer halfway through recording
            // (same place as rebuildAaResources).
            void requestProbeBake(uint64_t ownerId) { m_probePass.requestBake(ownerId); }
            void requestProbeBakeAll()              { m_probePass.requestBakeAll(); }
            // ms of the LAST bake (one probe or the whole batch), by timestamps.
            float lastProbeBakeMs() const { return m_probePass.lastBakeMs(); }
            int   probeCount() const      { return m_probePass.count(); }
            // GPU memory of the persistent captures of ONE probe, in bytes.
            // It does not count the capture cubemap, which is a single one for all of them.
            static constexpr uint64_t staticProbeMemoryBytes()
            {
                return ReflectionProbePass::probeMemoryBytes();
            }
            uint64_t probeMemoryBytes() const override { return staticProbeMemoryBytes(); }
            // ms of the last bake of ONE specific probe, or -1 if it was never baked.
            float probeBakeMs(uint64_t ownerId) const { return m_probePass.bakeMs(ownerId); }

            // Global switch: when off, not a dispatch of the mip chain is recorded
            // and the composition adds zero bloom (the LDR pass is NOT skipped,
            // since it is also the one that tonemaps).
            void  setBloomEnabled(bool v) override;
            void  setShadowResolution(int v) override;
            void  setPresentMode(PresentMode v) override;
            bool  presentModeSupported(PresentMode v) const override;
            // GPU cost of the bloom + composition of the last resolved frame, in
            // ms. 0 if the device does not support timestamps.
            float bloomGpuMs() const         { return m_bloomPass.gpuMs(); }

            // SSAO. Off by default: with the flag false neither the
            // depth pre-pass nor the two dispatches are recorded, and the AO map is set to 1.0
            // just once, so the image is the same as before the feature
            // and the GPU cost drops to zero.
            void  setSsaoEnabled(bool v);
            // GPU cost of the pre-pass + the two dispatches, in ms. 0 if the effect
            // is off or the device does not support timestamps.
            float ssaoGpuMs() const          { return m_ssaoPass.gpuMs(); }

            // GPU cost of SSR in ms: the two dispatches, plus the depth pre-pass
            // when it is SSR that requests it (with SSAO on, that pre-pass
            // is already counted by ssaoGpuMs and is not added twice here).
            float ssrGpuMs() const           { return m_ssrPass.gpuMs(); }

            // GPU cost of the fog dispatch in ms. 0 if it is off or the
            // device does not support timestamps.
            float fogGpuMs() const              { return m_fogPass.gpuMs(); }

            // ── Anti-aliasing ────────────────────────────────────────────────
            // MUTUALLY EXCLUSIVE modes: only one active at a time. None leaves the frame
            // exactly as before the feature (not one extra command).
            // The enum lives in RendererState: both backends read the same
            // mode. None no anti-aliasing, Fxaa morphological filter over the already
            // tonemapped LDR, Ssaa supersampling, Msaa multisample in the
            // rasterizer and Taa temporal accumulation with subpixel jitter.
            using AaMode = RendererState::AaMode;
            // Changing mode may require recreating resources (SSAA changes the size
            // of ALL the internal targets; MSAA, the number of samples of the
            // images, the render passes and the pipelines). That is not done here:
            // it is marked and the first following drawFrame resolves it, with the GPU already
            // idle. The parameters of each mode, on the other hand, travel by push
            // constant and take effect on the next frame without recreating anything.
            void   setAaMode(AaMode mode);
            // SSAA: resolution multiplier per axis. The cost grows with the
            // SQUARE, and the size is clamped to the device's maxImageDimension2D.
            void  setSsaaFactor(float v);
            // MSAA samples per pixel. Clamped to what the device supports
            // (framebufferColorSampleCounts & framebufferDepthSampleCounts).
            void setMsaaSamples(int v);
            int  maxMsaaSamples() const;
            // GPU cost of the active mode's OWN pass (FXAA, SSAA resolve,
            // TAA accumulation). In None and in MSAA it is 0: MSAA has no
            // pass of its own, its cost is spread across the scene and the composition,
            // and to see it you have to look at renderGpuMs().
            float motionBlurGpuMs() const override { return m_motionBlurPass.gpuMs(); }
            float aaGpuMs() const                 { return m_aaGpuMs; }
            // GPU cost of ALL the render without the UI: from the scene pass
            // to the last post pass. It is measured ALWAYS, also in None, which
            // is exactly what makes it useful: it is the reference against which the
            // real overhead of SSAA and MSAA is compared.
            // No hard limit: the vectors grow. What does not grow (since there
            // is a pool) is the number of live entries after a Play/Stop cycle.
            SlotUsage slotUsage() const override
            {
                SlotUsage u;
                u.objects = m_objects.size() - m_staticSlots.freeCount();
                u.skinned = m_skinnedObjects.size() - m_skinnedSlots.freeCount();
                return u;
            }
            float renderGpuMs() const             { return m_renderGpuMs; }

            // ── Performance panel instrumentation (editor only) ──────────────
            // Off by default: with the panel closed not one more timestamp is recorded
            // than there already was, nor are the counters touched.
            // The panel turns it on while it is visible and turns it off when it closes.
            void setPerfCaptureEnabled(bool on);
            bool perfCaptureEnabled() const       { return m_perfCapture; }
            // GPU cost of the shadow pass and the scene pass. They are 0 until
            // the capture has been on for two frames (they are read from frame N-2,
            // which is the one this slot's fence already waited for).
            float shadowGpuMs() const             { return m_shadowGpuMs; }
            float sceneGpuMs() const              { return m_sceneGpuMs; }
            // Counters of the last frame recorded with the capture on.
            // "Culled" are the static + skinned objects that the frustum left
            // out of the scene pass.
            int   statDrawCalls() const           { return m_statDrawCalls; }
            int   statInstances() const           { return m_statInstances; }
            int   statCulled() const              { return m_statCulled; }
            // Objects that were left without room in this frame's instance
            // SSBO and therefore without shadow or depth. It must ALWAYS
            // be 0: if it goes up, the capacity is wrongly sized. Before it was not even
            // counted and the symptom was a shadow that was not there (H23).
            int   statInstanceOverflow() const override { return m_statInstanceOverflow; }

            // ── Forward+ ─────────────────────────────────────────────────────
            // Radius PER light, in the same order as setLights. Empty (the usual case) =
            // all use the global radius above.
            void setLightRadii(const std::vector<float>& radii) { m_lightRadii = radii; }
            // GPU cost of the culling dispatch, in ms. 0 in Off.
            float forwardPlusGpuMs() const        { return m_fpPass.gpuMs(); }
            // Average lights per NON-EMPTY cell of the last resolved frame, and
            // number of cells that exceeded the per-cell maximum (those do
            // lose lights: it is the signal that the radius needs to be lowered).
            float forwardPlusAvgPerCell() const   { return m_fpPass.avgPerCell(); }
            uint32_t forwardPlusOverflowCells() const { return m_fpPass.overflowCells(); }

            // decoded: pixels that the worker already decoded for this mesh (nullptr
            // on the synchronous path). It queues all the uploads in the current pump's batch
            // and marks the object with the current ticket: it is not drawn until
            // flushPendingUploads() sends it and its fence signals.
            int addSkinnedMesh(const SkinnedMesh& mesh, const std::vector<DecodedImage>* decoded = nullptr);
            // Rebuilds ALL the GPU resources of the skinned object `index` from
            // `mesh`, in the same slot (the GameObject's skinnedRenderIndex
            // does not change). Necessary after adding or removing clips: the keyframes
            // live in SSBOs uploaded only once, and the GPU would have the old
            // list. It keeps transform, animTime and activeClip.
            void rebuildSkinnedMesh(int index, const SkinnedMesh& mesh);
            // Adds a new static mesh (buffers + textures + descriptor set) and
            // registers it in m_objects. Returns the index for GameObject::staticRenderIndex.
            // decoded: same contract as addSkinnedMesh (nullptr = synchronous path).
            int addStaticMesh(const Mesh& mesh, const std::vector<DecodedImage>* decoded = nullptr);
            // Rebuilds the MATERIAL of the static mesh `index` without moving it.
            // The whole contract is in EditorRenderer::rebuildStaticMesh, which
            // is what the other backend reads; here only what belongs to Vulkan: with
            // a single owner the three images of the shared entry are replaced
            // in place and it is re-keyed, and with more than one the object
            // splits off into its own entry.
            void rebuildStaticMesh(int index, const Mesh& mesh) override;
            // Closes and submits the current pump's batch. Call ONCE after
            // processing all the frame's results.
            void flushPendingUploads();
            // true if there are incomplete uploads: a batch open in m_pendingBatch
            // or batches still in flight. Consumed by the runtime (Task 10) to know
            // whether it must keep pumping before considering the load finished.
            bool hasPendingUploads() const;
            // Closes the pending batch and BLOCKS until all the uploads in
            // flight have completed and been reclaimed, so that newly
            // registered objects are visible in THIS frame. Reserved for rare
            // synchronous user-initiated transitions (Play->Stop restore,
            // undo/redo of a Create): there the vkDeviceWaitIdle stall
            // is acceptable and reproduces the immediate visibility prior to async
            // loading. DO NOT use in the async path of Load Scene (it has its modal +
            // per-frame pump; blocking would reintroduce the stall the modal avoids).
            void flushUploadsAndWait();
            void updateAnimation(int index, float deltaTime);
            // Pure sink: sets the clip and the time that the Animator already
            // computed on the CPU. It does not advance time, unlike
            // updateAnimation, which is still the path for objects WITHOUT
            // AnimatorComponent. The two do not step on each other: whoever has an Animator never
            // goes through updateAnimation.
            void setAnimationState(int index, uint32_t clipIndex, float animTime);
            // The pose of an Animator: up to 6 samples and the frozen one (see
            // AnimationPose). Each clip is clamped as in setAnimationState.
            void setAnimationPose(int index, const AnimationPose& pose);
            // The frame's IK constraints, already resolved and in model space
            // (see AnimationIk). count 0 = no IK.
            void setAnimationIk(int index, const AnimationIk& ik);
            void setSkinnedTransform(int index, const glm::mat4& transform);

            // ── Frustum culling ──────────────────────────────────────────────
            // The culling geometry lives in Renderer/Frustum.h since there is
            // a second backend that needs it; what remains here are the aliases and the
            // wrappers, which is how the tests and the rest of this
            // file get in.
            using Frustum = Culling::Frustum;
            static Frustum frustumFromViewProj(const glm::mat4& viewProj);
            static bool aabbVisible(const Frustum& frustum,
                                    const glm::vec3& localMin,
                                    const glm::vec3& localMax,
                                    const glm::mat4& model);
            // Radius of a sphere centered on the model's LOCAL ORIGIN that
            // contains the skinned mesh in ANY pose of ANY clip. The
            // rest-pose AABB is not enough: the compute deforms the vertices and a raised
            // arm sticks out of the box, so culling with it would make
            // the character disappear, the worst possible failure here.
            //
            // It does not evaluate any pose: it bounds bone by bone with the EXTREME
            // values of the keys (reach accumulated through the hierarchy x accumulated
            // scale, plus the radius of the vertex cloud each bone drags,
            // measured in its own space). That is why the bound also holds
            // for interpolated times: mix() does not leave the segment between
            // its extremes and slerp() returns a rotation, which does not change
            // norms. It is loose on purpose: false positive yes, false
            // negative never.
            //
            // Returns 0 if there is nothing to bound with (no bones or no vertices);
            // the caller treats it as "no bound" and does not cull.
            static float skinnedBoundRadius(const SkinnedMesh& mesh);
            // ── Draw batching by instancing ──────────────────────────────────
            // The grouping lives in Renderer/InstanceBatching.h since there is a
            // second backend that needs it, same as the culling in
            // Renderer/Frustum.h. These aliases and the delegate below stay
            // so as not to touch the callers nor instancing_tests.cpp.
            using InstanceBatch  = Batching::InstanceBatch;
            using BatchCandidate = Batching::BatchCandidate;
            static uint32_t buildInstanceBatches(const BatchCandidate* candidates,
                                                 size_t                count,
                                                 glm::mat4*            outTransforms,
                                                 uint32_t              outCapacity,
                                                 uint32_t              firstInstanceBase,
                                                 std::vector<InstanceBatch>& outBatches);

        private:

            // RenderObject, SkinnedMatGfx, SubMeshDraw, PushData and
            // SkinnedRenderObject live in RenderObjects.h since the probe bake
            // went out to its own pass: ReflectionProbePass receives them
            // through its Context to redraw the scene, and putting this header
            // inside a pass would be circular. They are in the same namespace, so
            // here they are still named unqualified.

            void createSwapChain(Window& window);
            void createImageViews();
            void createOffscreenRenderPass();
            // Game UI pass over the final, already anti-aliased image.
            void createUiRenderPass();
            // LDR pass that composes HDR + bloom, tonemaps and also hosts the
            // selection outline and the gizmos: those two have to stay
            // OUTSIDE the tonemap to keep coming out with their usual flat color,
            // and that is why they are no longer drawn in the scene pass.
            void createCompositeRenderPass();
            void createRenderPass();
            void createFramebuffers();
            void createOffscreenImages();
            void destroyOffscreenImages();
            // The two bindings of bloom_composite.frag (HDR scene + mip 0 of the
            // bloom). They go with the swapchain, same as the chain.
            void createCompositeSets();
            // The shared state package that BloomPass needs.
            BloomPass::Context bloomCtx();
            // Bloom mip chain + descriptor sets of the three steps. It goes
            // with the swapchain: the starting resolution is half the viewport,
            // so resizing recreates it entirely (destroyBloomImages first).
            void createBloomImages();
            void destroyBloomImages();
            // The composition pipeline and its layouts, plus the resolution of the
            // timestamp support. Size-independent, once in
            // initSceneResources.
            void createBloomPipelines();
            // Only the composition triangle's pipeline. It lives apart because
            // MSAA forces it to be rebuilt (it changes rasterizationSamples and the
            // render pass) without touching the layouts nor the pools.
            void recreateCompositePipeline();
            // SSAO and depth pre-pass. Both passes live in their classes; this
            // only assembles the shared state package of each.
            SsaoPass::Context         ssaoCtx();
            DepthPrepassPass::Context depthPrepassCtx();
            // Depth pre-pass of the scene + the two dispatches. camFrustum and fc
            // are the SAME as the frame's: the pre-pass has to cull with the same
            // criterion as the scene pass or the AO would darken against geometry
            // that is then not drawn.
            void recordSsaoPass(VkCommandBuffer cmd, const Frustum& camFrustum, const glm::mat4& proj);
            // Rewrites binding 7 (AO map) of all the descriptor sets already
            // allocated. Necessary after recreating the images with the swapchain: the
            // sets would point to destroyed views.
            void refreshSsaoDescriptors();
            // SSR. Same split as SSAO: the pipelines once, the
            // images and the sets with the swapchain (hung from
            // createOffscreenImages/destroyOffscreenImages).
            // The pass lives in SsrPass; this only assembles the shared state package
            // that it needs for each call.
            SsrPass::Context ssrCtx();
            // The walk of m_objects/m_skinnedObjects that decides whether there is anything
            // to reflect: the lists belong to the Renderer, so the loop
            // stays here and the result goes into the Context.
            bool anyObjectWithSsr() const;
            // Volumetric fog. Same split as SSR: the pipeline once,
            // the descriptor sets with the swapchain (they reference
            // m_hdrView and m_ssaoDepthView, which are recreated with it). The pass lives
            // in FogPass; this only assembles the shared state package that it
            // needs for each call.
            FogPass::Context fogCtx();
            // Camera motion blur. Same split as SSR: the pipeline once,
            // and the images and descriptor sets with the swapchain.
            // The pass lives in MotionBlurPass; this only assembles the shared
            // state package that it needs for each call.
            MotionBlurPass::Context motionBlurCtx();
            // A single write of binding 7 on `set`. Shared by
            // allocateObjectDescriptorSet, the skinned path and the refresh above.
            void writeSsaoBinding(VkDescriptorSet set, int frameIndex);
            // Anti-aliasing. The resolve pass is graphics (full-screen
            // triangle) and not compute: the swapchain is B8G8R8A8_SRGB and
            // Vulkan forbids storage images in sRGB formats.
            // Anti-aliasing. The RESOLVE pass lives in AaPass; what remains here are
            // the two query pools (the AA one also measures the whole frame)
            // and the multisample targets, which are attachments of the scene and
            // composition render passes.
            AaPass::Context aaCtx();
            void createAaQueryPools();
            void createMsaaImages();
            void destroyMsaaImages();
            // Rebuilds EVERYTHING that depends on the mode, with the GPU idle:
            // internal extent, images, and (only if the number of samples changes)
            // the scene and composition render passes and their pipelines. Called by
            // drawFrame when m_aaResourcesDirty is set.
            void rebuildAaResources();
            // Recomputes m_renderExtent from the mode and the SSAA factor,
            // clamping to maxImageDimension2D. Returns true if it changed.
            bool updateRenderExtent();
            // true if the active mode needs the composition to write to the
            // intermediate image instead of directly to m_offscreenImage.
            bool needsAaIntermediate() const;
            // The samples the images and pipelines must have NOW:
            // m_msaaSamples if the mode is Msaa, one otherwise.
            VkSampleCountFlagBits targetSampleCount() const;
            // Destroys and recreates the graphics pipelines that live in the
            // scene pass and the composition one. Only needed when changing
            // the number of samples: in Vulkan 1.0 rasterizationSamples is not
            // dynamic state. Those of shadows, depth pre-pass and resolve do not
            // go in: their render passes always stay at one sample.
            void recreateMsaaDependentPipelines();
            // Forward+. Same split as SSAO and SSR: layout, pool,
            // pipelines, queries and the buffers that do NOT depend on size (lights,
            // parameters, counters) once in init; the grid and the index list
            // with the swapchain.
            void createFpPipelines();
            void createFpBuffers();
            void destroyFpBuffers();
            // The culling dispatch of the active mode. It goes AFTER recordSsaoPass
            // (the tiled one reads the depth pre-pass that one records) and BEFORE the scene
            // pass, which is the one that consumes the grid. In Off it records nothing.
            void recordFpCullPass(VkCommandBuffer cmd, const glm::mat4& proj);
            // Dimensions of a mode's grid at the current INTERNAL resolution.
            // A single place: the buffer sizing, the parameter block and the
            // dispatch all call it, and if they disagreed cells outside would be read.
            ForwardPlusPass::Context fpCtx();
            void createCommandBuffers();
            void createSyncObjects();
            // Records the whole frame. It was a 755-line function in one piece
            // (H7): now it orchestrates, and each large pass lives in its method, as
            // recordShadowPass and recordSsaoPass already did. The extraction was
            // pure cut-and-paste; the only thing that changed is that the locals that
            // crossed from one block to another became parameters, and there you see at
            // a glance what before had to be traced by hand.
            void recordCommandBuffer(uint32_t imageIndex);
            // Inverted hull of the selected object, at the end of the scene
            // pass and before the skybox. camFrustum is the same as the frame's culling:
            // the highlighted object is evaluated again with the same
            // criterion, it is not given a free pass. No-op if there is no selection.
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
            // Allocates MAX_FRAMES consecutive sets and returns the POOL they come from,
            // which is what is needed later to free them. It chains a new pool
            // when the last one fills, instead of throwing: the pool was
            // sized ONCE with the startup meshes plus 128 of
            // margin, and past that margin scene loading fell over.
            //
            // It is chained instead of recreating a larger one because recreating it
            // would invalidate the sets of everything already loaded.
            //
            // VK_NULL_HANDLE if it could not be done.
            VkDescriptorPool allocateSharedSets(VkDescriptorSet* outSets);
            bool             addDescriptorPool();
            void createDescriptorSets();
            void updateUniformBuffer(uint32_t frameIndex);
            void createDepthResources();
            // Resolves mesh to a shared entry and leaves obj pointing to
            // it: it creates it (buffers + textures) only if no other object
            // had already uploaded that same mesh+material. Returns true if it had to
            // create it; the caller uses it to know whether the descriptor set
            // also has to be allocated for it.
            bool buildRenderObject(const Mesh& mesh, RenderObject& obj,
                                   TransferBatch* batch = nullptr,
                                   const std::vector<DecodedImage>* decoded = nullptr);
            // Writes the object's EFFECTIVE factors applying the rule
            // "with an ORM map the map rules". The two paths that set factors
            // (registering the object and the editor setter) go through here so
            // that they cannot disagree.
            static void setEffectiveFactors(RenderObject& obj, const SharedGpuMesh& gpu,
                                            float metallic, float roughness);
            // Fills a newly created entry. It is the body that used to be
            // in buildRenderObject, without the key resolution part.
            void createSharedGpuMesh(const Mesh& mesh, SharedGpuMesh& gpu,
                                     TransferBatch* batch,
                                     const std::vector<DecodedImage>* decoded);
            void allocateObjectDescriptorSet(SharedGpuMesh& gpu);
            void destroySharedGpuMesh(const SharedGpuMesh& gpu);
            // The shadow map lives in ShadowPass; this assembles its shared state
            // package (the two set layouts that shadow.vert declares).
            ShadowPass::Context shadowCtx();
            // The DRAWS of the N cascades: they stay here because they come from
            // m_objects/m_skinnedObjects and from the instance SSBO, whose cursor
            // is shared by this pass, the depth pre-pass and the scene one. The pass
            // sets the render pass, the pipeline and the index push.
            void recordShadowPass(VkCommandBuffer cmd);
            // Global IBL and reflection probes. Both passes live in their
            // classes; this only assembles the shared state package of each
            // one. The bake one is long on purpose: the capture REDRAWS the
            // scene, so it needs the whole offscreen pass and the object lists,
            // which belong to the Renderer and stay here.
            IblPass::Context             iblCtx();
            ReflectionProbePass::Context probeCtx();
            // The outline only needs on loan the meshes' layout (it uses
            // the same set 0 and the same push constants) and the COMPOSITION render
            // pass, which is where it is drawn so that the tonemap does not
            // touch its color.
            SelectionOutlinePass::Context outlineCtx();
            // The three skinning dispatches live in SkinningPass; this assembles
            // its shared state package (the lists, which belong to the Renderer).
            SkinningPass::Context skinningCtx();
            // The four GRAPHICS pipelines of the bone meshes. They stay
            // here because they depend on the number of MSAA samples and on the
            // scene render pass.
            void createSkinnedGraphicsPipelines();
            void destroySkinnedRenderObject(SkinnedRenderObject& obj);
            // Body shared by addSkinnedMesh and rebuildSkinnedMesh: creates
            // buffers, uploads SSBOs, allocates descriptor sets and loads textures onto
            // an already empty SkinnedRenderObject.
            void initSkinnedRenderObject(SkinnedRenderObject& obj, const SkinnedMesh& mesh,
                                         TransferBatch* batch = nullptr,
                                         const std::vector<DecodedImage>* decoded = nullptr);
            void removeStaticObject(int index);
            void removeSkinnedObject(int index);

            // They drop the reference / queue the destruction instead of
            // executing it. They are the ONLY allowed path: calling
            // destroySharedGpuMesh directly from a new call site would
            // destroy resources that other objects still use, and even if there were
            // none it would again need a vkDeviceWaitIdle that nobody would
            // remember to put.
            //
            // releaseRenderObject leaves obj.sharedIndex at -1 and only queues the
            // destruction when the last holder of the entry drops.
            void releaseRenderObject(RenderObject& obj);
            void queueDestroySkinnedRenderObject(SkinnedRenderObject& obj);

            // Effective camera of a frame. eye goes here because ubo.viewPos
            // feeds the specular: without it, in Play the highlights would be computed
            // from the editor camera's position.
            struct FrameCamera {
                glm::mat4 view;
                glm::mat4 proj;
                glm::vec3 eye;
            };

            // The CameraComponent's in Play (if the scene has one), the editor's
            // fly one in any other case. The only place where it is
            // decided: before, the projection was duplicated bare in
            // recordCommandBuffer and updateUniformBuffer.
            FrameCamera currentFrameCamera() const;

            // Evaluates the objects against `frustum` (Visibility::gatherCandidates)
            // and groups the visible ones into m_instanceBatches, writing their
            // transforms into this pass's own stretch inside the frame's
            // SSBO. The batches' firstInstance already come out with the base of
            // that stretch applied, so the caller only has to draw.
            //
            // It is called by the THREE passes that draw static geometry (scene,
            // shadows (once per cascade) and depth pre-pass), each with its own
            // frustum: the visible sets do not coincide, so each one
            // writes its own range and the m_instanceBuffers cursor goes after it.
            //
            // colorPass = false turns SSR off in the grouping key: the
            // passes that do not draw color do not read it and with a single value
            // fewer draws come out.
            void gatherAndBatch(const Frustum& frustum, bool colorPass);

            // Pass 1: the 3D scene into its own render target.
            void recordScenePass(VkCommandBuffer cmd, const FrameCamera& fc,
                                  const Frustum& camFrustum, bool perfStamp);
            // Bloom and composition: from the scene's HDR to the screen LDR. It
            // takes uiExtent and the UI counters because the SCREEN canvas
            // is drawn here, over the already composed image.
            void recordBloomAndComposite(VkCommandBuffer cmd, const FrameCamera& fc,
                                          const Frustum& camFrustum, VkExtent2D uiExtent,
                                          uint32_t uiScreenVertices, uint32_t uiScreenIndices);
            // Pass 2: the 2D UI over the swapchain image.
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

            // Offscreen render target (already tonemapped LDR result -> texture
            // sampled by the UI, or direct blit to the swapchain in headless).
            // It still has the swapchain's format: the scene is no longer drawn
            // here, the composition pass writes it.
            VkRenderPass                    m_offscreenRenderPass               = VK_NULL_HANDLE;
            VkImage                         m_offscreenImage[MAX_FRAMES]        = {};
            VkDeviceMemory                  m_offscreenMemory[MAX_FRAMES]       = {};
            VkImageView                     m_offscreenView[MAX_FRAMES]         = {};
            VkSampler                       m_offscreenSampler                  = VK_NULL_HANDLE;
            VkFramebuffer                   m_offscreenFramebuffer[MAX_FRAMES]  = {};

            // ── Game UI's own pass ───────────────────────────────────────────
            // The UI is drawn AFTER anti-aliasing, over the final image and
            // at one sample. Before it went inside the composition pass, that is
            // BEFORE the AA: with FXAA the text was over-smoothed and with TAA
            // it left a trail when moving, while the DirectX 12 backend (which
            // draws it over the already resolved back buffer) came out clean. It is
            // also what makes the UI be drawn only once at the output
            // resolution with SSAA instead of being supersampled for nothing.
            //
            // loadOp LOAD and both layouts in SHADER_READ_ONLY: the image already
            // carries the resolved scene and has to stay as it was for the
            // editor panel and for the runtime blit.
            VkRenderPass                    m_uiRenderPass                      = VK_NULL_HANDLE;
            VkFramebuffer                   m_uiFramebuffer[MAX_FRAMES]         = {};
            // Opaque UI handle, not a VkDescriptorSet: it is produced by
            // UiLayer::registerUiTexture and the Renderer only stores it to
            // hand it back. It is uint64_t because that contract no longer knows Vulkan.
            uint64_t                        m_offscreenDescSet[MAX_FRAMES]      = {};

            // ── HDR + bloom ──────────────────────────────────────────────────
            // Floating-point format of the scene target and the bloom chain. The
            // scene pass has to come out in HDR without clamping to [0,1] or the
            // bloom threshold would have nothing above it to extract.
            static constexpr VkFormat       kHdrFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
            // Scene target (what used to be m_offscreenImage).
            VkImage                         m_hdrImage[MAX_FRAMES]              = {};
            VkDeviceMemory                  m_hdrMemory[MAX_FRAMES]             = {};
            VkImageView                     m_hdrView[MAX_FRAMES]               = {};

            // Bloom mip chain: BloomPass has the whole thing. Its sampler and
            // mip 0 come out through its getters, which is what the composition
            // set below needs.
            BloomPass                       m_bloomPass;

            // Composition pass: HDR + bloom -> tonemap -> m_offscreenImage.
            VkRenderPass                    m_compositeRenderPass               = VK_NULL_HANDLE;
            VkFramebuffer                   m_compositeFramebuffer[MAX_FRAMES]  = {};
            VkDescriptorSetLayout           m_compositeDescLayout               = VK_NULL_HANDLE;
            VkDescriptorPool                m_compositeDescPool                 = VK_NULL_HANDLE;
            VkDescriptorSet                 m_compositeSets[MAX_FRAMES]         = {};
            VkPipelineLayout                m_compositePipelineLayout           = VK_NULL_HANDLE;
            VkPipeline                      m_compositePipeline                 = VK_NULL_HANDLE;

            // Device properties shared by all the passes with queries.
            // Resolved by createBloomPipelines, which is the first to ask for a
            // pool.
            bool                            m_timestampsSupported               = false;
            float                           m_timestampPeriod                   = 0.0f;

            // ── SSAO + depth pre-pass ─────────────────────────────────
            // The pre-pass depth is NOT the SSAO's even though it was born with
            // it: it is shared by SSR, TAA, tiled Forward+, fog and
            // motion blur. That is why they are two classes, and the NEAREST sampler of
            // that depth comes out through DepthPrepassPass::sampler().
            DepthPrepassPass                m_depthPrepass;
            SsaoPass                        m_ssaoPass;

            // ── SSR ──────────────────────────────────────────────────────────
            // Reflection image, sampler, pipelines, sets and queries are its own;
            // the Renderer only owns it and decides when to create, record and
            // destroy. The sampler and the query pool come out through its getters:
            // they are used by the motion blur and the depth pre-pass, which do not belong to the pass.
            SsrPass                         m_ssrPass;
            // It stays in the Renderer because it is WRITTEN by the depth pre-pass
            // (recordSsaoPass) and not by the pass: it marks that it left this frame's
            // timestamps [0,1] written. SsrPass only takes the measurement as
            // good if that is so (otherwise the pair would not have been written and the read
            // would give NOT_READY).
            bool                            m_ssrStampedPrepass                 = false;

            // ── Volumetric fog ───────────────────────────────────────────────
            // Pipeline, sets and timing queries are its own; the Renderer only
            // owns it and decides when to create, record and destroy.
            FogPass                         m_fogPass;

            // ── Motion blur ──────────────────────────────────────────────────
            // Images, sets and pipeline are its own; the Renderer only owns it and
            // decides when to create, record and destroy.
            MotionBlurPass                  m_motionBlurPass;

            // ── Anti-aliasing ────────────────────────────────────────────────
            // REQUESTED mode: what the user chose and what aaMode() returns.
            // It may be a frame ahead of the resources.
            // BUILT mode: the one that corresponds to the images, framebuffers and
            // pipelines that exist RIGHT NOW. It is the one that rules when recording the
            // frame. The two are separate because setAaMode is called from the UI, and
            // the UI is built AFTER the point at which resources can be recreated:
            // without this distinction, the click's frame would be recorded in the
            // new mode with the old one's framebuffers (or with none).
            AaMode                          m_aaActiveMode                      = AaMode::None;
            // Set by setAaMode/setSsaaFactor/setMsaaSamples when the change
            // touches resources (target size or number of samples). Consumed
            // by drawFrame BEFORE recording anything, with vkDeviceWaitIdle: in the
            // middle of a frame in flight an image cannot be destroyed.
            bool                            m_aaResourcesDirty                  = false;
            // The requested shadow map is not the one that is set up. It is handled between
            // frames, same as m_aaResourcesDirty: recreating it with the GPU
            // running would release an image that the frame in flight is reading.
            bool                            m_shadowResourcesDirty              = false;
            void rebuildShadowResources();
            // Rewrites binding 3 (the shadow map) in ALL the descriptor
            // sets: those of each shared mesh and those of each character
            // material. Without this, after a resize they point to a dead view.
            void refreshShadowDescriptors();
            // INTERNAL resolution of the render. Equal to m_swapChainExtent except in
            // SSAA, where it is m_swapChainExtent * m_ssaaFactor. It governs ALL the
            // intermediate targets (HDR scene, depth, SSAO, SSR, bloom, the AA
            // intermediate image) and their viewports. m_swapChainExtent stays for
            // what really has the window's size: the swapchain, the
            // UI pass, the blit and m_offscreenImage.
            VkExtent2D                      m_renderExtent                      = {};
            // Size at which the image is PRESENTED: the editor panel's, or the
            // swapchain's when nobody has set it (runtime and headless). It is the
            // size of m_offscreenImage and of the AA resolve pass.
            // REQUESTED: the last thing the panel reported. Same as with the mode,
            // it comes from the UI and may be a frame ahead of the resources.
            VkExtent2D                      m_viewportExtent                    = {};
            // BUILT: the size with which the images and framebuffers exist now.
            // It is the one that rules when recording, and the only one that can
            // appear in a renderArea: asking for an area larger than the framebuffer
            // is invalid.
            VkExtent2D                      m_viewportActive                    = {};
            VkExtent2D effectiveViewport() const
            {
                return m_viewportActive.width > 0 && m_viewportActive.height > 0
                     ? m_viewportActive : m_swapChainExtent;
            }
            // Intermediate image, TAA history, framebuffers, sets and the
            // three resolve pipelines: AaPass has all of that. From it
            // also come the composition's alternate framebuffer and the
            // two view-proj matrices that the motion blur consumes.
            AaPass                          m_aaPass;


            // MSAA. m_msaaSamples is what the user REQUESTS; m_aaSampleCount is
            // what is built right now in the images, the render
            // passes and the pipelines: the two only coincide when the active mode
            // is Msaa and it has already been rebuilt.
            VkSampleCountFlagBits           m_aaSampleCount                     = VK_SAMPLE_COUNT_1_BIT;
            // Multisample color of the scene: it is RESOLVED onto m_hdrImage when
            // closing the pass, so SSAO, SSR, bloom and composition
            // keep reading exactly the same single-sample image as today.
            // It carries no STORAGE: multisample storage images require the feature
            // shaderStorageImageMultisample, which is not requested.
            VkImage                         m_msaaHdrImage[MAX_FRAMES]          = {};
            VkDeviceMemory                  m_msaaHdrMemory[MAX_FRAMES]         = {};
            VkImageView                     m_msaaHdrView[MAX_FRAMES]           = {};
            // Multisample color of the composition, with resolve onto
            // m_offscreenImage. It exists so that the outline and the gizmos, which are
            // drawn in that pass, also come out smoothed: the depth they load
            // is the scene's multisample one and there is no way to resolve it in
            // Vulkan 1.0 (VK_KHR_depth_stencil_resolve is 1.2).
            VkImage                         m_msaaLdrImage[MAX_FRAMES]          = {};
            VkDeviceMemory                  m_msaaLdrMemory[MAX_FRAMES]         = {};
            VkImageView                     m_msaaLdrView[MAX_FRAMES]           = {};

            // Own queries: reusing the bloom ones would mix the tonemap's cost
            // with the anti-aliasing's. Two pairs per frame: [0,1] the
            // mode's own pass, [2,3] the full render without UI.
            VkQueryPool                     m_aaQueryPool                       = VK_NULL_HANDLE;
            bool                            m_aaQueryPending[MAX_FRAMES]        = {};
            float                           m_aaGpuMs                           = 0.0f;
            float                           m_renderGpuMs                       = 0.0f;
            uint32_t                        m_aaMeasuredFrames                  = 0;

            // ── Performance panel ────────────────────────────────────────────
            // Four queries per frame in flight: [0,1] shadow pass, [2,3]
            // scene pass. They are only reset and written if m_perfCapture, and
            // the results are read without WAIT_BIT from the slot of two frames ago.
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
            // The whole pass (layout, pipelines, buffers, sets and queries) is held
            // by ForwardPlusPass. Its descriptor set is set 2 of the scene
            // pipeline and its layout goes into that pipeline layout, so both
            // come out through its getters.
            ForwardPlusPass                 m_fpPass;
            // REQUESTED mode (the one forwardPlusMode() returns) and FROZEN mode
            // of the frame. Same reason as in AA: setForwardPlusMode is called
            // from the UI, which is built midway through drawFrame, and the parameter
            // block that pbr.frag reads is written only once per frame. Without
            // this separation, a click could leave the frame with the grid of one
            // mode and the read of the other.
            FpMode                          m_fpActiveMode                      = FpMode::Off;
            std::vector<float>              m_lightRadii;

            // Where a POINT light aims, which has no direction of its own: the
            // center of the scene. It is recomputed once per frame and read by the
            // TWO consumers of the key light's direction (the cascades and the
            // fog), which have to see exactly the same value or the
            // in-scattering points one way and the shadow map is built
            // toward another (H65). Empty scene: it stays at the origin, which is
            // what was always done before.
            glm::vec3                       m_sceneCenter                       {0.0f};

            VkSemaphore                     m_imageAvailable[MAX_FRAMES]        = {};
            std::vector<VkSemaphore>        m_renderFinished;
            VkFence                         m_inFlight[MAX_FRAMES]              = {};
            int                             m_currentFrame                      = 0;
            VkPipelineLayout                m_pipelineLayout                    = VK_NULL_HANDLE;
            VkPipeline                      m_pipeline                          = VK_NULL_HANDLE;
            VkPipeline                      m_wireframePipeline                 = VK_NULL_HANDLE;
            bool                            m_framebufferResized                = false;
            // Presentation modes that THIS device supports, cached when creating
            // the swapchain (which is where the surface and physicalDevice already exist). FIFO
            // does not go in: the spec always guarantees it.
            bool                            m_mailboxDisponible                 = false;
            bool                            m_immediateDisponible               = false;
            bool                            m_headless                          = false;
            VkDescriptorSetLayout           m_descriptorSetLayout               = VK_NULL_HANDLE;
            VkBuffer                        m_uniformBuffers[MAX_FRAMES]        = {};
            VkDeviceMemory                  m_uniformBuffersMemory[MAX_FRAMES]  = {};
            void*                           m_uniformBuffersMapped[MAX_FRAMES]  = {};
            // Chain of pools, not a single one: see allocateSharedSets. Each
            // SharedGpuMesh and each SkinnedMatGfx remembers which one is its own.
            std::vector<VkDescriptorPool>   m_descriptorPools;
            static constexpr uint32_t       kSharedSetsPerPool                  = 128 * MAX_FRAMES;
            // ── Per-instance transforms SSBO (set 1, binding 0) ──────────────
            // Eight members and three methods until H8; now its own class
            // carries them, with the same contract as the passes. What was gained is not
            // the count: the cursor and the pointer arithmetic are no longer
            // within the reach of whoever writes, and its overflow guard was
            // copied by hand in three places of this file.
            InstanceBuffers                 m_instanceBuffers;
            static_assert(MAX_FRAMES == InstanceBuffers::kFrames,
                          "The Renderer's frames in flight and InstanceBuffers' must "
                          "match: the sets of the extra frames would be born without a buffer");
            // Scratch reused between frames and between passes: gatherAndBatch
            // runs once per cascade plus two more (depth pre-pass and scene)
            // in each frame, and must not allocate anything on that path.
            std::vector<BatchCandidate>     m_batchCandidates;
            std::vector<InstanceBatch>      m_instanceBatches;
            // Visibility of m_skinnedObjects for THIS frame, in the same order and
            // indexed the same. It is computed only once at the start of
            // recordCommandBuffer because two places read it: the compute (which goes
            // first in the command buffer) and the drawing. Sharing the same
            // decision is what avoids the pop: if the compute skipped an
            // object that is then drawn, its outputVertexBuffer would keep
            // the pose of the last frame in which it was visible.
            std::vector<uint8_t>            m_skinnedVisible;
            // The three methods of the instance SSBO went with it to
            // InstanceBuffers (H8): create/destroy and the buffer growth. The
            // `ctx` of a field, like the passes'.
            InstanceBuffers::Context instanceCtx() { return InstanceBuffers::Context{ m_gpu }; }
            VkImage                         m_depthImage                        = VK_NULL_HANDLE;
            VkDeviceMemory                  m_depthImageMemory                  = VK_NULL_HANDLE;
            VkImageView                     m_depthImageView                    = VK_NULL_HANDLE;
            glm::vec3                       m_cameraTarget{0.0f};
            float                           m_cameraDistance{5.0f};
            glm::mat4                       m_viewMatrix{1.0f};
            Camera                          m_camera;
            std::vector<Light>              m_lights;

            // Cascaded shadow map. The texture array, its two pipelines and the
            // cascade split live in ShadowPass; what the Renderer still
            // needs from it are the view and the sampler (binding 3 of each
            // descriptor set), its pipeline layout (which it lends to the depth
            // pre-pass) and the cascade matrices (which it copies into the UBO).
            ShadowPass                      m_shadowPass;

            // ── IBL and probes ─────────────────────────────────────────────
            // The two global cubemaps live in IblPass and the probes in
            // ReflectionProbePass, which reuses its convolution pipelines. The only
            // thing the Renderer still needs from them are the two
            // views and the global IBL's sampler, which it writes into bindings 5
            // and 6 of each descriptor set (allocateObjectDescriptorSet and the skinned
            // path).
            IblPass                         m_iblPass;
            ReflectionProbePass             m_probePass;
            // Selection outline: the four pipelines and the highlighted
            // object. Only used by the editor, and the runtime leaves it at (-1,-1)
            // for its whole life. Its DRAWS stay in recordSelectionOutline, which
            // needs the Renderer's object lists; same split as
            // ShadowPass and DepthPrepassPass.
            SelectionOutlinePass            m_outlinePass;

            // The bake copies frame 0's UBO (lights, cascades and their shadow map)
            // and only replaces view/proj: without a previous frame that buffer is
            // garbage, so the requests wait. It stays here because it is
            // written by updateUniformBuffer; the pass reads it through its Context.
            bool                            m_uboWritten[MAX_FRAMES] {};

            // Compute skinning: the three pipelines, their layout, their pool and their
            // descriptor sets live in SkinningPass. From it also come the
            // compute sets that initSkinnedRenderObject allocates.
            SkinningPass          m_skinningPass;
            // The graphics ones, on the other hand, stay: they depend on MSAA.
            VkPipeline            m_skinnedGfxPipeline        = VK_NULL_HANDLE;
            VkPipeline            m_skinnedWireframePipeline  = VK_NULL_HANDLE;
            std::vector<SkinnedRenderObject> m_skinnedObjects;

            std::vector<RenderObject> m_objects;

            // Free slots of the two vectors above. Deleting an object
            // cannot compact (the indices are recorded in each GameObject),
            // so the slot is recycled on the next add instead of
            // letting them grow endlessly (H19).
            SlotPool m_staticSlots;
            SlotPool m_skinnedSlots;

            // GPU resources shared by the static objects. Objects
            // keep an index here; the table keeps them alive as long as any holder
            // remains. (Skinned ones do not share: their output SSBOs
            // are written by the compute per instance.)
            SharedGpuMeshCache m_sharedMeshes;
            // Material image of a skinned character, shared among those
            // that come from the same FBX (before, 218 MB per character: VRAM
            // ran out between 33 and 40). The views are still per character.
            struct MaterialImage
            {
                VkImage        image = VK_NULL_HANDLE;
                VkDeviceMemory mem   = VK_NULL_HANDLE;
                // Format the image was created with (decided by resolveSrgb): the
                // view of each character that shares it has to declare it the same.
                VkFormat       format = VK_FORMAT_UNDEFINED;
                bool operator==(const MaterialImage& o) const { return image == o.image && mem == o.mem; }
            };
            SharedTextureCache<MaterialImage> m_skinnedTextures;

            // Read-only buffers of a character (B6): input vertices, indices,
            // keyframes and bone infos. Identical across clones of the same model,
            // so they are shared by content key (skinnedGeometryKey) with refcount,
            // same cache as the textures. The per-character buffers (pose, output
            // vertices) stay in SkinnedRenderObject.
            struct SkinnedGeometry
            {
                VkBuffer       posBuf   = VK_NULL_HANDLE, rotBuf = VK_NULL_HANDLE, scaleBuf = VK_NULL_HANDLE,
                               boneBuf  = VK_NULL_HANDLE, vtxBuf = VK_NULL_HANDLE, idxBuf   = VK_NULL_HANDLE;
                VkDeviceMemory posMem   = VK_NULL_HANDLE, rotMem = VK_NULL_HANDLE, scaleMem = VK_NULL_HANDLE,
                               boneMem  = VK_NULL_HANDLE, vtxMem = VK_NULL_HANDLE, idxMem   = VK_NULL_HANDLE;
                bool operator==(const SkinnedGeometry& o) const { return vtxBuf == o.vtxBuf && idxBuf == o.idxBuf; }
            };
            SharedTextureCache<SkinnedGeometry> m_skinnedGeometry;
            void destroySkinnedGeometry(const SkinnedGeometry& g);

            // Open batch where the current pump's uploads land. It is sent in
            // flushPendingUploads() and moves to m_inFlightBatches.
            std::unique_ptr<TransferBatch> m_pendingBatch;
            struct InFlightBatch { uint64_t ticket; std::unique_ptr<TransferBatch> batch; };
            std::vector<InFlightBatch>     m_inFlightBatches;
            uint64_t                       m_nextUploadTicket      = 1;
            uint64_t                       m_lastCompletedTicket   = 0;

            DeferredDeleteQueue m_deferredDeletes;

            UiLayer* m_ui = nullptr;
            Skybox   m_skybox;
            SplashScreen m_splash;

            // Game UI. It lives in Core: the exported runtime draws these same
            // canvases inside the composition pass, with nothing from the editor.
            //
            // One per CanvasComponent of the scene. syncUiCanvases() pairs them
            // by ownerId (matchUiCanvasSlots), so reordering the canvases
            // in the hierarchy does not reset the tree nor the cache of the one that
            // has not moved. m_uiBatch is a SINGLE shared one: its per-frame
            // vertex/index buffers are sub-allocated (UiSpriteBatch::beginFrame
            // + record) so that each canvas writes into its own slot of the
            // same VkBuffer instead of overwriting the previous one.
            std::vector<std::unique_ptr<UiCanvasSlot>> m_uiSlots;
            // The frame's WORLD ones, already sorted far to near by
            // sortWorldCanvasesBackToFront. A member and not a local of the recording
            // loop so as not to reallocate the vector every frame; it is cleared by the
            // sort function itself.
            std::vector<UiCanvasSlot*>                m_uiWorldOrder;
            // Fallback of uiCanvas() when the scene has no screen
            // canvas. Persistent and empty: returning a reference to a temporary
            // would leave the gizmos reading dead memory.
            UiCanvas      m_uiCanvasFallback;
            UiSpriteBatch m_uiBatch;
            std::vector<std::unique_ptr<UiTextureAtlas>> m_uiAtlases;
            std::vector<std::unique_ptr<UiFont>>         m_uiFonts;
            // By PATH: the same image requested twice is the same atlas, not
            // two. Without this each editor query uploaded another texture and another
            // descriptor set, and the UiSpriteBatch pool is 32. It is also what
            // lets the editor touch the sprites of the atlas that is being
            // drawn instead of those of a copy of its own.
            std::unordered_map<std::string, UiTextureAtlas*> m_uiAtlasByPath;
            // ImGui handle per atlas, cached: ImGui_ImplVulkan_AddTexture
            // reserves a descriptor set PER CALL, and calling it every frame eats
            // the editor's pool in seconds.
            std::unordered_map<const UiTextureAtlas*, uint64_t> m_uiAtlasImGuiId;
            // Shared thumbnail atlas of the Content Browser (see
            // EditorRenderer::uiThumbnailAtlasId). Created the first time it is
            // requested; if it fails, it is not retried every frame.
            VkImage        m_thumbImage    = VK_NULL_HANDLE;
            VkDeviceMemory m_thumbMemory   = VK_NULL_HANDLE;
            VkImageView    m_thumbView     = VK_NULL_HANDLE;
            uint64_t       m_thumbImGuiId  = 0;
            bool           m_thumbFailed   = false;
            void           destroyThumbAtlas();
            GameObject* m_sceneRoot = nullptr;
            Scene* m_scene = nullptr;
    };
}