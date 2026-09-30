#pragma once

#include "DonTopo/Core/AnimationIk.h"
#include "DonTopo/Core/AnimationPose.h"
#include "DonTopo/Renderer/RendererState.h"
#include "DonTopo/Renderer/ThumbnailAtlas.h"
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

    // What the editor needs from a render backend, on top of the
    // quality settings that both already share (RendererState).
    //
    // It exists so that the panels do not talk to the Vulkan Renderer by its
    // name: any backend that implements this can drive the editor. What
    // it does not know how to do it says by returning zero or doing nothing: a
    // backend without reflection probes answers 0 probes, and the panel shows it
    // as is instead of hiding the section.
    //
    // What does NOT go in here: the lifecycle (init, drawFrame, shutdown) nor the native
    // handles. That is carried by whoever builds the backend, which knows which one
    // it is; the editor only consumes the scene, the viewport and the metrics.
    class EditorRenderer : public RendererState
    {
        public:
            virtual ~EditorRenderer() = default;

            // ── Lifecycle ───────────────────────────────────────────────────
            // What the RUNTIME needs to drive a backend without knowing which one
            // it is. They are NOT pure: they carry the "this backend does not
            // do that" behavior, which is right for a splash that does not exist or for
            // a backend that does not distinguish the two startup phases. The editor
            // does not go through here; it is built by main, which does know which backend there is.
            //
            // Phase 1: being able to present. Phase 2: what depends on the meshes
            // (camera auto-fit and scene resources). A backend that does
            // everything at once implements the first and leaves the second.
            virtual void initPresentation(Window& window) { (void)window; }
            virtual void initSceneResources(const std::vector<Mesh>& meshes) { (void)meshes; }
            virtual void initSkybox(const std::array<std::string, 6>& facePaths) { (void)facePaths; }

            // No editor window: one that does not set up ImGui on its own has
            // nothing to shut down.
            virtual void setHeadless(bool headless) { (void)headless; }

            // Startup splash. false = there is none, and the caller carries on without it instead
            // of waiting for a fade that never arrives.
            virtual bool beginSplash(const std::string& logoPath) { (void)logoPath; return false; }
            virtual void drawSplashFrame(float alpha) { (void)alpha; }

            virtual void drawFrame(Window& window) { (void)window; }
            virtual void notifyResize() {}
            virtual void shutdown() {}

            // Is anything left to upload from the asynchronous path? One that uploads synchronously
            // has no queue and answers no.
            virtual bool hasPendingUploads() const { return false; }

            // Atlas and fonts of the 2D UI. The owner is the backend; whoever
            // asks for them keeps the pointer. nullptr = it could not be loaded, and the
            // widget is drawn with its flat color.
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

            // ── Scene ───────────────────────────────────────────────────────
            // Render index of the object, or -1. decoded are the pixels a
            // worker already decompressed; nullptr is the synchronous path.
            virtual int  addStaticMesh(const Mesh& mesh,
                                       const std::vector<DecodedImage>* decoded = nullptr) = 0;
            virtual void rebuildSkinnedMesh(int index, const SkinnedMesh& mesh)            = 0;
            // Rebuilds the GPU resources of an already registered STATIC mesh, without
            // moving its render index: transform, visibility and SSR are
            // kept, and the undo commands that store that index remain
            // valid. It is what the texture change from Properties uses.
            //
            // Objects that shared mesh and material with this one do NOT change
            // appearance: changing the material changes the dedup key, so
            // this object splits off from the group and the others keep the
            // previous entry, intact.
            //
            // `mesh` is the same already registered mesh WITH THE MATERIAL CHANGED, and
            // that is the only thing this rebuilds: the backend takes the geometry as
            // good and reuses what it already uploaded. Changing the vertices through here
            // is NOT supported (see the warning of Renderer::rebuildStaticMesh);
            // for that, re-register the object.
            virtual void rebuildStaticMesh(int index, const Mesh& mesh)                    = 0;
            virtual void registerGameObject(GameObject* node)                              = 0;
            virtual void removeGameObject(GameObject* node)                                = 0;
            virtual void removeMeshComponent(GameObject* node)                             = 0;

            // A texture that could not be loaded: it is replaced by the "something is missing"
            // one so that the object stays visible and the failure is noticed.
            enum class TextureSlot { Diffuse, Normal, MetallicRoughness };
            virtual void replaceStaticTextureWithMissing(int renderIndex, TextureSlot slot) = 0;

            // Closes the pending submissions and waits: used by the transitions
            // that need to see the result in THIS frame (undoing a
            // Create, leaving Play).
            virtual void flushUploadsAndWait() = 0;

            // Closes the frame's upload batch without waiting: whatever lands
            // will be seen as soon as its fence signals.
            virtual void flushPendingUploads() = 0;

            // Recomputes the depth range with what is loaded. Without
            // this, changing the startup scene clips the background.
            virtual void refitCameraRange() = 0;

            // What is drawn with an outline; -1 in both for none.
            virtual void setOutlineTarget(int staticIndex, int skinnedIndex) = 0;

            // ── Frame scene ─────────────────────────────────────────────────
            // What the main loop sets on each pass: which camera is used to
            // draw, which lights there are and where each object is.
            virtual void setScene(Scene* scene)                   = 0;
            virtual void setSceneRoot(GameObject* root)           = 0;
            virtual void setCamera(const Camera& camera)          = 0;
            virtual void setLights(const std::vector<Light>& lights) = 0;
            // Reach radius per light, in the same order as setLights. Used by
            // the per-cell split; empty = the global radius.
            virtual void setLightRadii(const std::vector<float>& radii) = 0;

            virtual int  addSkinnedMesh(const SkinnedMesh& mesh,
                                        const std::vector<DecodedImage>* decoded = nullptr) = 0;

            virtual void setTransform(size_t objectIndex, const glm::mat4& transform) = 0;
            virtual void setSkinnedTransform(int index, const glm::mat4& transform)   = 0;
            virtual void setObjectMeshVisible(size_t objectIndex, bool visible)       = 0;
            virtual void setSkinnedMeshVisible(int index, bool visible)               = 0;
            // How much the object reflects; 0 = nothing.
            virtual void setObjectSsr(size_t objectIndex, float strength) = 0;
            virtual void setSkinnedSsr(int index, float strength)         = 0;

            // PBR factors of the object, without uploading or rebuilding ANYTHING: they are two floats
            // per object that travel by push constant, like the SSR strength
            // above. That is the whole reason it exists: the previous path
            // (rebuildStaticMesh) did waitForGpu and re-uploaded three
            // textures, so a slider could only apply on release; through here
            // it can be dragged live.
            //
            // If the object's material brings an ORM MAP, the backend ignores these
            // values and leaves both at 1.0: the texture rules, which is the same
            // rule that addStaticMesh and Vulkan already apply when registering. Calling
            // here with a map set is not an error, it simply does nothing.
            //
            // Index out of range: no-op, like the setters above.
            virtual void setObjectMaterialFactors(size_t objectIndex, float metallic,
                                                  float roughness) = 0;

            // Advances the animation time of a character, or sets the one that an
            // Animator already computed on the CPU.
            virtual void updateAnimation(int index, float deltaTime)                    = 0;
            virtual void setAnimationState(int index, uint32_t clipIndex, float animTime) = 0;
            // Cross-fade: the TWO clips in flight and the blend weight.
            // weight 0 = prevClip only, 1 = clipIndex only. It is a superset
            // of setAnimationState (which is equivalent to weight 1), but it stays
            // separate so as not to touch the signature already used by objects without a blend.
            // Since row 13 of the animation audit it is the whole POSE: up to 4
            // samples (clip, time, weight), the weight of the frozen pose and the
            // freeze request, plus the root mode (see AnimationPose).
            virtual void setAnimationPose(int index, const AnimationPose& pose) = 0;
            // IK of the frame (row 15 of the animation audit): up to 4
            // constraints already resolved against the scene, with the target and the
            // pole in MODEL space. count 0 = no IK, and it has to be called
            // all the same to turn off the previous frame's.
            virtual void setAnimationIk(int index, const AnimationIk& ik) = 0;

            // Releases what was left pending deletion when the GPU allows it.
            virtual void tickDeferredDeletes() = 0;

            // ── Viewport ────────────────────────────────────────────────────
            virtual void     setViewportSize(uint32_t width, uint32_t height) = 0;
            virtual uint32_t renderWidth() const                              = 0;
            virtual uint32_t renderHeight() const                             = 0;
            // OUTPUT size. With SSAA it is not the internal render's, and it is the
            // space in which the UI canvas is resolved (buildDrawData) and in
            // which the mouse has to be fed to UiCanvas::updateInput.
            virtual uint32_t uiWidth() const                                  = 0;
            virtual uint32_t uiHeight() const                                 = 0;

            // Identifier of an already loaded atlas FOR THE INTERFACE (what
            // ImGui::Image understands as a texture), or 0 if that atlas is not
            // uploaded. Requested by the sprite editor, which needs to show the
            // image the sub-rects are drawn on. The value is made by
            // each backend in its own way and cached: registering the same texture
            // every frame exhausts the descriptor pool in seconds.
            virtual uint64_t uiAtlasTextureId(const UiTextureAtlas* atlas)    = 0;

            // Shared thumbnail atlas of the Content Browser: ONE texture of
            // kThumbAtlasSize² with a single ImGui descriptor (ImGui's pools
            // are 48 sets in Vulkan and 16 slots in D3D12: one texture per
            // thumbnail does not fit). They are not pure: a backend without support answers
            // "no", and the grid keeps its colored icon.
            //
            // Atlas id (what ImGui::AddImage understands as a texture), or 0 if the
            // backend does not support it or could not create it. It is created on the first
            // call and afterwards always returns the same value.
            virtual uint64_t uiThumbnailAtlasId() { return 0; }
            // Copies the cells into the atlas, ALL of them in a single GPU wait. false =
            // it could not be done and nothing reliable was copied (the whole batch fails).
            virtual bool uploadUiThumbnails(const ThumbnailTile* tiles, size_t count)
            {
                (void)tiles;
                (void)count;
                return false;
            }

            virtual float    viewportAspect() const                           = 0;

            // The interface layer drawn on top. The backend calls it inside
            // the frame; the editor sets it once.
            virtual void setUiLayer(UiLayer* ui) = 0;

            // Tree of the game's 2D interface, the SCREEN one. A backend that
            // does not draw it yet returns its own empty one: the editor edits it
            // all the same.
            virtual UiCanvas& uiCanvas() = 0;

            // ALL the SCREEN canvases, in INPUT PRIORITY order: the topmost
            // first (the last one drawn). It is what is needed to split the mouse and keyboard
            // among several canvases with dispatchUiInput, and so that the editor's click
            // selects what the user sees ON TOP.
            //
            // uiCanvas() is no good for that: it returns just ONE (the first screen
            // one), and with it the buttons of a second canvas are drawn
            // but have no hover, no state colors, no Click.
            virtual void screenUiCanvases(std::vector<UiCanvas*>& out) = 0;

            // The canvas of ONE GameObject, by its id, or nullptr if it has none. Used
            // by the gizmo of the SELECTED canvas: `uiCanvas()` gave it the
            // FIRST screen one, so selecting a second canvas painted
            // the first one's rect.
            //
            // It goes apart from screenUiCanvases and not inside it because that one feeds
            // dispatchUiInput, whose signature belongs to the UI core
            // (`vector<UiCanvas*>`): putting the ownerId in it would force the core to
            // know the Renderer's types, or unpacking a second
            // list per frame in the three loops.
            virtual const UiCanvas* uiCanvasOf(uint64_t ownerId) const = 0;

            // Builds the live tree of EACH canvas in the scene. Replaces the
            // collect + syncUiWidgets that the three loops used to repeat
            // (runtime and sandbox x2); three copies of the same thing is how they
            // get out of sync.
            virtual void syncUiCanvases(const std::vector<UiCanvasBinding>& bindings) = 0;

            // Looks up a node by name in ALL the canvases, not just the screen
            // one. Used by the editor's nine widget gizmos: without this,
            // a button inside a world canvas would be left without a gizmo and nothing
            // would say so.
            virtual const UiElement* findUiNode(const std::string& name) const = 0;

            // ── Anti-aliasing with resources behind it ──────────────────────
            // The mode and the samples live in RendererState, but changing them
            // moves images and pipelines, and each backend knows that.
            virtual void  setAaMode(AaMode mode)   = 0;
            virtual void  setMsaaSamples(int v)    = 0;
            virtual int   maxMsaaSamples() const   = 0;
            virtual void  setSsaaFactor(float v)   = 0;
            // The getter is provided by RendererState; here only the switch, which is
            // the one that moves the targets.
            virtual void  setSsaoEnabled(bool v)   = 0;

            // Bloom is turned off by releasing its image chain, so the
            // switch cannot be a simple state bool either.
            // The getter is already provided by RendererState: here only the
            // switch is needed, which is the one that moves resources.
            virtual void  setBloomEnabled(bool v)  = 0;

            // The shadow map side moves the image, its views and the
            // framebuffers, so it cannot be a simple state int either.
            // The getter is already provided by RendererState. Each backend decides WHEN to
            // redo it: with the GPU idle and afterwards rewriting the
            // descriptors that pointed at the old map.
            virtual void  setShadowResolution(int v) = 0;

            // Presentation mode. It recreates the swapchain, so it cannot
            // be a simple state value either. The getter is already provided by RendererState.
            //
            // The backend FALLS BACK TO VSYNC if the requested mode is not supported: it is the
            // only one Vulkan always guarantees and the one DXGI gives without an extension.
            // presentMode() keeps returning what was REQUESTED even if it fell back,
            // so that project.json keeps the user's intent if they
            // later open the project on a machine that does support it.
            virtual void  setPresentMode(PresentMode v) = 0;

            // Which modes THIS device can provide. The UI queries it to
            // disable those that cannot, with the reason in a tooltip, instead of
            // hiding them: if the core supports N options, the UI offers N and the
            // nuance is documented.
            //
            // Vsync always returns true.
            virtual bool  presentModeSupported(PresentMode v) const = 0;

            // ── Reflection probes ───────────────────────────────────────────
            virtual void  requestProbeBake(uint64_t ownerId) = 0;
            virtual void  requestProbeBakeAll()              = 0;
            virtual int   probeCount() const                 = 0;
            virtual float lastProbeBakeMs() const            = 0;
            // GPU memory of ONE probe, in bytes. Virtual and not a shared
            // constant because the two backends store DIFFERENT things per probe:
            // Vulkan counts irradiance and prefilter (its capture
            // cubemap is a single one for all of them), and D3D12 also adds the
            // capture one, which there is per probe. The panel called the
            // static constexpr of the Vulkan class (H51), so under
            // DirectX 12 it was not only the figure of the backend that was not running: it
            // underestimated it.
            virtual uint64_t probeMemoryBytes() const = 0;
            virtual float probeBakeMs(uint64_t ownerId) const = 0;

            // ── Object slots ────────────────────────────────────────────────
            // How many render entries are alive and how many fit. Deleting an
            // object does not compact the vector (the indices are recorded in each
            // GameObject) but it does return its slot, so a repeated
            // Play/Stop cycle has to leave `used` where it was. Being able to
            // see it is what tells recycling apart from a leak (H19, H43).
            //
            // capacity == 0 means "no hard limit": the backend grows the
            // vector. Vulkan is like that; D3D12 has 512 and 16, which are the sizes
            // the descriptor heap was divided with.
            struct SlotUsage {
                size_t objects         = 0;
                size_t objectCapacity  = 0;
                size_t skinned         = 0;
                size_t skinnedCapacity = 0;
            };
            // Not pure virtual: a backend that does not keep the count returns
            // zeros and the panel treats it as "no data", instead of forcing
            // both to implement it the day a third one is added.
            virtual SlotUsage slotUsage() const { return {}; }

            // ── Metrics ─────────────────────────────────────────────────────
            // In GPU milliseconds of the last measured frame. Zero if the
            // backend does not take them.
            virtual void  setPerfCaptureEnabled(bool on)      = 0;
            virtual float renderGpuMs() const                 = 0;
            virtual float ssaoGpuMs() const                   = 0;
            virtual float ssrGpuMs() const                    = 0;
            virtual float bloomGpuMs() const                  = 0;
            virtual float fogGpuMs() const                    = 0;
            virtual float aaGpuMs() const                     = 0;
            // Motion blur. It was the ONLY unmeasured pass in ANY backend, so
            // it was turned on blindly despite being one of the expensive ones: one more dispatch plus
            // a copy of the whole image.
            virtual float motionBlurGpuMs() const { return 0.0f; }
            virtual float sceneGpuMs() const                  = 0;
            virtual float shadowGpuMs() const                 = 0;
            virtual float forwardPlusGpuMs() const            = 0;

            // Counts of the last frame: draws sent, instances drawn and
            // objects discarded by the frustum.
            virtual int statDrawCalls() const = 0;
            virtual int statInstances() const = 0;
            virtual int statCulled() const    = 0;
            // Objects that were left without room in the frame's instance SSBO, and therefore
            // without shadow or depth. It must ALWAYS be 0: if it goes up, the capacity is wrongly
            // sized. It is not pure because not all backends split their instances this way; one
            // that does not measure it answers 0 and the panel shows nothing (H23).
            virtual int statInstanceOverflow() const { return 0; }
            virtual float forwardPlusAvgPerCell() const       = 0;
            virtual uint32_t forwardPlusOverflowCells() const = 0;
    };
}
