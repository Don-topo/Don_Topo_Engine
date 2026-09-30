#pragma once
#include <glm/glm.hpp>
#include <cstdint>
#include <cstddef>

namespace DonTopo {

    // How frames are delivered to the screen.
    //
    //   Vsync     waits for the refresh. No tearing and no burning GPU, but it pins
    //             the frame rate to the monitor's. It is the default and the ONLY
    //             one that all platforms guarantee.
    //   Mailbox   triple buffer: it does not wait and does not break the image, but it draws
    //             frames that get discarded. Vulkan only; DXGI has no
    //             equivalent.
    //   Immediate does not wait: the rate goes above the refresh and tearing
    //             appears. It is the only mode with which the real cost of a frame
    //             can be MEASURED; with Vsync everything comes out at 16 ms and it looks like
    //             nothing you do matters.
    //
    // The value lives here, but changing it recreates the swapchain, so the
    // real switch is a virtual of EditorRenderer. Same as
    // shadowResolution and msaaSamples.
    enum class PresentMode : int { Vsync = 0, Mailbox = 1, Immediate = 2 };

    // Scalar state of render quality and effects: ambient weight and
    // bloom, SSAO, SSR, fog, anti-aliasing and Forward+ parameters.
    //
    // It lives apart because NONE of this depends on the graphics API: these are values
    // that travel by push constant or by UBO and that both backends (Vulkan
    // (Renderer) and DirectX 12 (D3D12Renderer)) need equally. By sharing
    // this base, an exported game's options menu and the editor panels
    // talk about the same get/set whatever the backend, instead of
    // duplicating the method pairs and their default values.
    //
    // ONLY what is assigned and read goes in here. Any setter that also
    // triggers work (recreating targets, marking resources dirty, clearing an
    // image) stays in the backend, which is the one that knows what has to be redone.
    class RendererState {
        public:
            // Global weight of the IBL ambient. 1.0 = the environment exactly as the
            // cubemap gives it. It travels through the UBO, so it changes on the next frame
            // without recomputing anything.
            void  setAmbientIntensity(float v) { m_ambientIntensity = v; }
            float ambientIntensity() const     { return m_ambientIntensity; }
            // Global switch: turning it off does NOT destroy the precomputed IBL, it only
            // sends 0 in the UBO. It can be turned on again without recomputing anything.
            void  setAmbientEnabled(bool v) { m_ambientEnabled = v; }
            bool  ambientEnabled() const    { return m_ambientEnabled; }

            // ── Bloom ────────────────────────────────────────────────────────
            // HDR bloom. All three travel by push constant of the bloom
            // pipelines (NOT by the UBO: only 2 floats were left there and the block is
            // declared in 5 shaders), so they change on the next frame without
            // recreating anything. intensity = 0 leaves the image exactly the same as
            // before the feature.
            // The switch, like the parameters: the same panel turns the
            // effect on in both backends. Vulkan adds its own logic when
            // turning it off (it releases the image chain), but the value lives here.
            bool  bloomEnabled() const        { return m_bloomEnabled; }
            void  setBloomEnabledFlag(bool v) { m_bloomEnabled = v; }
            void  setBloomThreshold(float v) { m_bloomThreshold = v; }
            float bloomThreshold() const     { return m_bloomThreshold; }
            void  setBloomKnee(float v)      { m_bloomKnee = v; }
            float bloomKnee() const          { return m_bloomKnee; }
            void  setBloomIntensity(float v) { m_bloomIntensity = v; }
            float bloomIntensity() const     { return m_bloomIntensity; }

            // ── SSAO ─────────────────────────────────────────────────────────
            // The four parameters travel by push constant of the SSAO
            // pipeline (NOT by the UBO: only two floats were left and the block is
            // declared in 5 shaders), so they change on the next frame
            // without recreating anything.
            // The switch, like the parameters: the same panel turns the
            // effect on in both backends. Vulkan adds its own logic when
            // turning it off (leaving the map at identity), but the value lives here.
            bool  ssaoEnabled() const        { return m_ssaoEnabled; }
            void  setSsaoEnabledFlag(bool v) { m_ssaoEnabled = v; }
            void  setSsaoRadius(float v)     { m_ssaoRadius = v; }
            float ssaoRadius() const         { return m_ssaoRadius; }
            void  setSsaoBias(float v)       { m_ssaoBias = v; }
            float ssaoBias() const           { return m_ssaoBias; }
            void  setSsaoIntensity(float v)  { m_ssaoIntensity = v; }
            float ssaoIntensity() const      { return m_ssaoIntensity; }
            void  setSsaoPower(float v)      { m_ssaoPower = v; }
            float ssaoPower() const          { return m_ssaoPower; }

            // ── SSR ──────────────────────────────────────────────────────────
            // SSR (screen-space reflections). Global switch; in addition
            // each GameObject carries its own strength, and with the switch on
            // but NO object marked, nothing is recorded either. The parameters
            // travel by their own push constant (SsrPush), not by the UBO.
            void  setSsrEnabled(bool v)      { m_ssrEnabled = v; }
            bool  ssrEnabled() const         { return m_ssrEnabled; }
            void  setSsrMaxDistance(float v) { m_ssrMaxDistance = v; }
            float ssrMaxDistance() const     { return m_ssrMaxDistance; }
            void  setSsrThickness(float v)   { m_ssrThickness = v; }
            float ssrThickness() const       { return m_ssrThickness; }
            void  setSsrMaxSteps(int v)      { m_ssrMaxSteps = v; }
            int   ssrMaxSteps() const        { return m_ssrMaxSteps; }
            void  setSsrEdgeFade(float v)    { m_ssrEdgeFade = v; }
            float ssrEdgeFade() const        { return m_ssrEdgeFade; }
            void  setSsrIntensity(float v)   { m_ssrIntensity = v; }
            float ssrIntensity() const       { return m_ssrIntensity; }

            // ── Volumetric fog ───────────────────────────────────────────────
            // Volumetric fog: exponential by height with in-scattering from
            // the key light. Global switch; when off it records not a single command and
            // the image comes out identical. The parameters travel by their own
            // push constant (FogPush), not by the UBO.
            void  setFogEnabled(bool v)         { m_fogEnabled = v; }
            bool  fogEnabled() const            { return m_fogEnabled; }
            void  setFogDensity(float v)        { m_fogDensity = v; }
            float fogDensity() const            { return m_fogDensity; }
            void  setFogHeightFalloff(float v)  { m_fogHeightFalloff = v; }
            float fogHeightFalloff() const      { return m_fogHeightFalloff; }
            void  setFogBaseHeight(float v)     { m_fogBaseHeight = v; }
            float fogBaseHeight() const         { return m_fogBaseHeight; }
            void  setFogScatter(const glm::vec3& v) { m_fogScatter = v; }
            const glm::vec3& fogScatter() const { return m_fogScatter; }
            void  setFogAnisotropy(float v)     { m_fogAnisotropy = v; }
            float fogAnisotropy() const         { return m_fogAnisotropy; }
            void  setFogSteps(int v)            { m_fogSteps = v; }
            int   fogSteps() const              { return m_fogSteps; }

            // ── Motion blur ──────────────────────────────────────────────────
            // Camera motion blur by reprojection: the velocity of each pixel
            // comes from the depth plus the previous frame's matrix, the same one the
            // TAA already uses. Global switch; when off it records not a single dispatch and
            // the image comes out identical. The parameters travel by their own
            // push constant (MotionBlurPush), not by the UBO.
            void  setMotionBlurEnabled(bool v)     { m_motionBlurEnabled = v; }
            bool  motionBlurEnabled() const        { return m_motionBlurEnabled; }
            void  setMotionBlurIntensity(float v)  { m_motionBlurIntensity = v; }
            float motionBlurIntensity() const      { return m_motionBlurIntensity; }
            void  setMotionBlurMaxRadius(float v)  { m_motionBlurMaxRadius = v; }
            float motionBlurMaxRadius() const      { return m_motionBlurMaxRadius; }
            void  setMotionBlurSamples(int v)      { m_motionBlurSamples = v; }
            int   motionBlurSamples() const        { return m_motionBlurSamples; }

            // ── Anti-aliasing ────────────────────────────────────────────────
            // The parameters of each mode travel by push constant and take
            // effect on the next frame without recreating anything. The active mode,
            // on the other hand, is chosen by the backend: changing it may require recreating
            // resources.
            // FXAA
            void  setFxaaSubpix(float v)          { m_fxaaSubpix = v; }
            float fxaaSubpix() const              { return m_fxaaSubpix; }
            void  setFxaaEdgeThreshold(float v)   { m_fxaaEdgeThreshold = v; }
            float fxaaEdgeThreshold() const       { return m_fxaaEdgeThreshold; }
            void  setFxaaEdgeThresholdMin(float v){ m_fxaaEdgeThresholdMin = v; }
            float fxaaEdgeThresholdMin() const    { return m_fxaaEdgeThresholdMin; }
            // TAA: history weight (0 = current frame only, no accumulation)
            // and subpixel jitter amplitude in pixels.
            void  setTaaFeedback(float v)         { m_taaFeedback = v; }
            float taaFeedback() const             { return m_taaFeedback; }
            void  setTaaJitterScale(float v)      { m_taaJitterScale = v; }
            float taaJitterScale() const          { return m_taaJitterScale; }

            // ── Cascaded shadows ─────────────────────────────────────────────
            // Both travel to the cascade split (computeCascades) and take
            // effect on the next frame without recreating anything: the shadow map does not
            // change size, only WHICH piece of the world each cascade covers changes. The
            // number of cascades does NOT go in here: it is SHADOW_CASCADES
            // and it has to be worth the same as the array of the UBO block that
            // 5 shaders declare and as the layers of the texture array.
            //
            // Maximum reach of the shadows. It is the setting that shows the most: the
            // 4 cascades divide this distance among them, so lowering it concentrates
            // the same texels in less world and sharpens the shadow up close, at the
            // cost of there being no shadow beyond.
            void  setShadowDistance(float v) { m_shadowDistance = v; }
            float shadowDistance() const     { return m_shadowDistance; }
            // Blend between the logarithmic split (1) and the uniform one (0). The
            // logarithmic gives resolution where it is seen, close up; the uniform keeps
            // the last cascade from covering almost everything. 0.75 leans toward the
            // logarithmic, which is what is wanted with large distances.
            void  setCascadeLambda(float v) { m_cascadeLambda = v; }
            float cascadeLambda() const     { return m_cascadeLambda; }

            // Side of the shadow map, in texels. Unlike the two above,
            // changing it MOVES RESOURCES (the image, its views and the
            // framebuffers), so the real switch is a virtual of
            // EditorRenderer and only the value lives here, same as with
            // msaaSamples and bloom.
            int  shadowResolution() const        { return m_shadowResolution; }
            void setShadowResolutionFlag(int v)  { m_shadowResolution = v; }

            // Supersampling factor: it is drawn at this multiple of the output size
            // and the resolve pass averages. Like msaaSamples, the VALUE
            // lives here and the switch that moves the targets is a virtual of
            // EditorRenderer. Before, each backend kept its own and they could
            // diverge from the one persisted by project.json.
            float ssaaFactor() const           { return m_ssaaFactor; }
            void  setSsaaFactorFlag(float v)   { m_ssaaFactor = v; }

            // Presentation mode. See PresentMode. What was REQUESTED, which does not have
            // to be what the device supports: whoever applies it falls back to Vsync if
            // the mode is not available, and presentModeSupported() says which ones
            // exist so the UI can disable the others with its reason instead of
            // hiding them.
            PresentMode presentMode() const              { return m_presentMode; }
            void        setPresentModeFlag(PresentMode v) { m_presentMode = v; }

            // How many lights the SCENE has, which is not the same as how many
            // illuminate. Scene::collectLights keeps the first
            // MAX_LIGHTS and discards the rest SILENTLY (it is a limit of the UBO
            // block, not a scene error), so without this number there is no
            // way to know what is being lost other than counting by hand.
            //
            // Here and not in a backend virtual because it does not depend on the API:
            // it is set by whoever assembles the frame, the only one that sees the total, and
            // it is read equally by the editor panel and the options menu of an
            // exported game.
            size_t sceneLightTotal() const        { return m_sceneLightTotal; }
            void   setSceneLightTotal(size_t v)   { m_sceneLightTotal = v; }

            // ── Forward+ ─────────────────────────────────────────────────────
            // GPU light culling. MUTUALLY EXCLUSIVE modes. Off leaves the frame
            // exactly as before the feature: not a dispatch, and pbr.frag
            // loops over the UBO's MAX_LIGHTS as always.
            // Anti-aliasing. The mode and the MSAA samples are requested by the user
            // from the same panel for both backends; what each one has
            // BUILT right now (images, pipelines) is its own business, because
            // changing it requires recreating resources with the GPU idle.
            enum class AaMode : int
            {
                None = 0,
                Fxaa = 1,
                Ssaa = 2,
                Msaa = 3,
                Taa  = 4,
            };
            // Wireframe mode: turned on by the editor's View menu and valid for
            // both backends, so the value lives here.
            bool isWireframeMode() const        { return m_wireframeMode; }
            void setWireframeMode(bool enabled) { m_wireframeMode = enabled; }

            AaMode aaMode() const            { return m_aaMode; }
            void   setAaModeFlag(AaMode m)   { m_aaMode = m; }
            int    msaaSamples() const       { return m_msaaSamples; }
            void   setMsaaSamplesFlag(int v) { m_msaaSamples = v; }

            enum class FpMode : int
            {
                Off       = 0,
                Tiled     = 1,  // 2D grid of 16x16 tiles with the tile's maximum depth
                Clustered = 2,  // 3D grid of 64x64 pixels x 24 logarithmic Z slices
            };
            // Changes on the NEXT frame and recreates nothing: both modes
            // share buffers (sized to the larger of the two grids), so
            // the only thing that changes is what is recorded and the parameter
            // block. The value that rules during a frame is frozen in
            // m_fpActiveMode right after the UI.
            void   setForwardPlusMode(FpMode mode) { m_fpMode = mode; }
            FpMode forwardPlusMode() const         { return m_fpMode; }
            // Default radius of ALL the lights that do not bring their own. It is the
            // data the culling needs and that Light does not carry: putting it in the
            // struct would change the std140 layout of the UBO, which 5 shaders declare.
            void  setForwardPlusLightRadius(float v) { m_fpLightRadius = v; }
            float forwardPlusLightRadius() const     { return m_fpLightRadius; }

        protected:
            float                           m_ambientIntensity{1.0f};
            bool                            m_ambientEnabled{true};

            float                           m_bloomThreshold                    = 1.0f;
            float                           m_bloomKnee                         = 0.5f;
            float                           m_bloomIntensity                    = 0.05f;

            bool                            m_wireframeMode                     = false;
            AaMode                          m_aaMode                            = AaMode::None;
            int                             m_msaaSamples                       = 4;
            bool                            m_bloomEnabled                      = true;
            bool                            m_ssaoEnabled                       = false;
            float                           m_ssaoRadius                        = 0.5f;
            float                           m_ssaoBias                          = 0.025f;
            float                           m_ssaoIntensity                     = 1.0f;
            float                           m_ssaoPower                         = 1.0f;

            bool                            m_ssrEnabled                        = false;
            float                           m_ssrMaxDistance                    = 8.0f;
            float                           m_ssrThickness                      = 0.5f;
            int                             m_ssrMaxSteps                       = 32;
            float                           m_ssrEdgeFade                       = 0.1f;
            float                           m_ssrIntensity                      = 1.0f;

            bool                            m_motionBlurEnabled                 = false;
            float                           m_motionBlurIntensity               = 1.0f;
            float                           m_motionBlurMaxRadius               = 32.0f;
            int                             m_motionBlurSamples                 = 12;

            bool                            m_fogEnabled                        = false;
            float                           m_fogDensity                        = 0.02f;
            float                           m_fogHeightFalloff                  = 0.02f;
            float                           m_fogBaseHeight                     = 0.0f;
            glm::vec3                       m_fogScatter                        {0.6f, 0.7f, 0.9f};
            float                           m_fogAnisotropy                     = 0.6f;
            int                             m_fogSteps                          = 32;

            // Values of the PC quality preset of FXAA 3.11.
            float                           m_fxaaSubpix                        = 0.75f;
            float                           m_fxaaEdgeThreshold                 = 0.166f;
            float                           m_fxaaEdgeThresholdMin              = 0.0833f;

            float                           m_taaFeedback                       = 0.9f;
            float                           m_taaJitterScale                    = 1.0f;

            // The values it was drawn with before this became
            // adjustable: a project without the new keys looks the same.
            float                           m_shadowDistance                    = 500.0f;
            float                           m_cascadeLambda                     = 0.75f;
            int                             m_shadowResolution                  = 2048;
            float                           m_ssaaFactor                        = 2.0f;
            PresentMode                     m_presentMode                       = PresentMode::Vsync;
            size_t                          m_sceneLightTotal                   = 0;

            FpMode                          m_fpMode                            = FpMode::Off;
            float                           m_fpLightRadius                     = 2000.0f;
    };
}
