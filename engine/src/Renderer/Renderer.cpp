#include "DonTopo/Renderer/IkBlock.h"
#include "DonTopo/Renderer/PoseBlock.h"
#include "DonTopo/Renderer/MeshClock.h"
#include "DonTopo/Renderer/Renderer.h"
#include "DonTopo/Renderer/Gizmos.h"
#include "DonTopo/Core/GameObject.h"
#include "DonTopo/Core/Scene.h"
#include "DonTopo/Core/CameraComponent.h"
#include <GLFW/glfw3.h>
#include <cstring>
#include <stdexcept>
#include "DonTopo/Core/Window.h"
#include <algorithm>
#include <fstream>
#include "DonTopo/Renderer/Vertex.h"
#include "DonTopo/Renderer/PipelineDefaults.h"
#include <glm/gtc/matrix_transform.hpp>
#include "DonTopo/Renderer/UniformBufferObject.h"
#include "DonTopo/Renderer/SelectionOutline.h"
#include "DonTopo/Renderer/SkinnedMeshPacking.h"
#include "DonTopo/Renderer/MaterialTextureSource.h"
#include <limits>
#include <cmath>
#include <chrono>

namespace DonTopo {

    Renderer::~Renderer()
    {
        shutdown();
    }

    // In headless there is no editor to press Play: the runtime starts playing
    // from frame 0.
    bool Renderer::isPlaying() const { return m_headless || (m_ui && m_ui->isPlaying()); }

    void Renderer::init(Window& window, const std::vector<Mesh>& meshes)
    {
        // Kept as the sum of the two phases, in the same order as
        // before (see initPresentation/initSceneResources for the details of the
        // split). The editor (Sandbox) calls init() and does not change; the
        // runtime calls the two phases separately to slip the splash
        // in between.
        initPresentation(window);
        initSceneResources(meshes);
    }

    void Renderer::initPresentation(Window& window)
    {
        // Gizmos::kFramesInFlight is used to size per-in-flight-frame buffers
        // inside Gizmos; it must always match Renderer::MAX_FRAMES. MAX_FRAMES
        // is private, so this static_assert lives here (member context) instead
        // of at file level.
        static_assert(Gizmos::kFramesInFlight == MAX_FRAMES,
            "Gizmos::kFramesInFlight must match Renderer::MAX_FRAMES");
        // Same case: MotionBlurPass sizes its images and its sets per
        // in-flight frame without being able to see MAX_FRAMES.
        static_assert(MotionBlurPass::kFramesInFlight == MAX_FRAMES,
            "MotionBlurPass::kFramesInFlight must match Renderer::MAX_FRAMES");
        static_assert(FogPass::kFramesInFlight == MAX_FRAMES,
            "FogPass::kFramesInFlight must match Renderer::MAX_FRAMES");
        static_assert(SsrPass::kFramesInFlight == MAX_FRAMES,
            "SsrPass::kFramesInFlight must match Renderer::MAX_FRAMES");
        static_assert(AaPass::kFramesInFlight == MAX_FRAMES,
            "AaPass::kFramesInFlight must match Renderer::MAX_FRAMES");
        static_assert(SsaoPass::kFramesInFlight == MAX_FRAMES,
            "SsaoPass::kFramesInFlight must match Renderer::MAX_FRAMES");
        static_assert(DepthPrepassPass::kFramesInFlight == MAX_FRAMES,
            "DepthPrepassPass::kFramesInFlight must match Renderer::MAX_FRAMES");

        // Phase 1: the bare minimum to be able to present a frame (splash included).
        // The camera auto-fit and the scene resources (pipelines, shadow,
        // compute, UI-independent descriptors such as offscreen, meshes)
        // live in initSceneResources because they depend on `meshes` or on
        // resources created right there.
        m_gpu.init(window.getNativeWindow());
        // BEFORE any render pass, image or pipeline: the number of
        // samples of the active AA mode goes into the creation of all three. Without
        // this, a default mode other than None would be built with one sample
        // and MSAA would do nothing, silently.
        m_aaActiveMode  = aaMode();
        m_aaSampleCount = targetSampleCount();
        createSwapChain(window);

        createImageViews();
        createDepthResources();
        createOffscreenRenderPass();
        createCompositeRenderPass();
        createUiRenderPass();
        // The gizmos go in the composition pass, not the scene one: there the
        // color is already tonemapped and their lines come out with exactly the flat
        // color they declare, same as before the bloom.
        Gizmos::init(m_gpu, m_compositeRenderPass, m_swapChainFormat, m_aaSampleCount);
        // AA passes: they only depend on m_swapChainFormat, same as the
        // composition one, so they survive resizes (what gets recreated are their
        // images and framebuffers, in createAaImages).
        m_aaPass.createRenderPasses(aaCtx());
        createRenderPass();
        createFramebuffers();
        // createCommandBuffers/createSyncObjects only depend on the device and
        // the command pool (createCommandBuffers) or on the device and
        // m_swapChainImages.size() (createSyncObjects); nothing from
        // initSceneResources (descriptor sets, pipelines, mesh) touches them
        // during init. They are moved up here, relative to the original, so that they
        // are ready in phase 1 along with the rest of what is needed to
        // present.
        createCommandBuffers();
        createSyncObjects();
        // needs m_renderPass + m_swapChainImages.size(); it does not depend on
        // anything in initSceneResources, so it moves here (it used to live in the
        // middle of the original init) so that the splash can draw with
        // the UI already operational if needed.
        if (!m_headless && m_ui)
        {
            // The handles travel as integers: UiLayer no longer includes vulkan.h,
            // because the same editor has to be able to draw itself with DirectX 12.
            UiLayer::InitInfo info{};
            info.api            = UiLayer::GraphicsApi::Vulkan;
            info.window         = window.getNativeWindow();
            info.instance       = reinterpret_cast<uint64_t>(m_gpu.instance());
            info.physicalDevice = reinterpret_cast<uint64_t>(m_gpu.physicalDevice());
            info.device         = reinterpret_cast<uint64_t>(m_gpu.device());
            info.queueFamily    = m_gpu.graphicsFamily();
            info.queue          = reinterpret_cast<uint64_t>(m_gpu.graphicsQueue());
            info.imageCount     = (uint32_t)m_swapChainImages.size();
            info.renderPass     = reinterpret_cast<uint64_t>(m_renderPass);
            m_ui->initUi(info);
        }
    }

    void Renderer::refitCameraRange()
    {
        // Range floor: a tiny scene (or one with everything at the same point)
        // would give far ~0 and not even the skybox would be visible. 200 leaves far=600, which
        // amply covers the camera the editor opens with (z=300 looking at the origin), and
        // near=0.2, which does not clip small props.
        constexpr float kMinCameraDistance = 200.0f;

        glm::vec3 bMin( std::numeric_limits<float>::max());
        glm::vec3 bMax(-std::numeric_limits<float>::max());
        bool      any = false;

        for (const RenderObject& obj : m_objects)
        {
            const SharedGpuMesh* gpu = m_sharedMeshes.get(obj.sharedIndex);
            if (!gpu || !gpu->hasBounds) continue;

            // The 8 corners of the local AABB taken to world space: with the object
            // rotated or scaled, the mesh's axis-aligned box no longer bounds it.
            for (int c = 0; c < 8; ++c)
            {
                const glm::vec3 corner((c & 1) ? gpu->aabbMax.x : gpu->aabbMin.x,
                                       (c & 2) ? gpu->aabbMax.y : gpu->aabbMin.y,
                                       (c & 4) ? gpu->aabbMax.z : gpu->aabbMin.z);
                const glm::vec3 world = glm::vec3(obj.transform * glm::vec4(corner, 1.0f));
                bMin = glm::min(bMin, world);
                bMax = glm::max(bMax, world);
            }
            any = true;
        }

        for (const SkinnedRenderObject& obj : m_skinnedObjects)
        {
            if (!obj.hasBounds) continue;
            // The bound of a skinned mesh is a sphere in local space (valid for any pose):
            // its center is taken in world space and the radius unscaled. Approximate on
            // purpose: this only sets near/far, it culls nothing.
            const glm::vec3 center(obj.transform[3]);
            bMin = glm::min(bMin, center - glm::vec3(obj.boundRadius));
            bMax = glm::max(bMax, center + glm::vec3(obj.boundRadius));
            any = true;
        }

        // Nothing boundable: keep the current range instead of leaving it at infinities.
        // This is what happens with the empty scene of a newly created project.
        if (!any) return;

        m_cameraTarget = (bMin + bMax) * 0.5f;
        const float maxDim =
            glm::max(bMax.x - bMin.x, glm::max(bMax.y - bMin.y, bMax.z - bMin.z));
        m_cameraDistance = glm::max(maxDim * 1.2f, kMinCameraDistance);
    }

    void Renderer::initSceneResources(const std::vector<Mesh>& meshes)
    {
        // Auto-fit camera to mesh bounding box (needs `meshes`; that is why it
        // lives here and not in initPresentation).
        glm::vec3 bMin( std::numeric_limits<float>::max());
        glm::vec3 bMax(-std::numeric_limits<float>::max());

        for(auto& mesh : meshes)
        {
            for (auto& v : mesh.vertices)
            {
                bMin = glm::min(bMin, v.pos);
                bMax = glm::max(bMax, v.pos);
            }
        }

        // Without a single mesh with vertices (empty scene, or all of them empty) the
        // extremes stay as they came out (bMin at +max and bMax at -max), and
        // then maxDim is -inf. From there it goes to m_cameraDistance and to the depth
        // range derived from it, and the editor starts with a
        // projection of infinities that draws nothing. refitCameraRange() does
        // guard against this case; this path did not.
        if (bMin.x > bMax.x)
        {
            bMin = glm::vec3(-1.0f);
            bMax = glm::vec3( 1.0f);
        }

        m_cameraTarget   = (bMin + bMax) * 0.5f;
        float maxDim     = glm::max(bMax.x - bMin.x, glm::max(bMax.y - bMin.y, bMax.z - bMin.z));
        m_cameraDistance = maxDim * 1.2f;

        // BEFORE createPipeline and createShadowResources: both pipeline
        // layouts declare m_instanceBuffers.descLayout() as set 1.
        m_instanceBuffers.create(instanceCtx());
        createDescriptorSetLayout();
        // BEFORE createPipeline: the scene pipeline layout declares
        // m_fpDescLayout as set 2. The buffers that DO depend on the size (the
        // grid) are created later by createFpBuffers, from createOffscreenImages.
        // The timestamps: device properties shared by all the passes
        // with queries. This pass runs BEFORE createBloomPipelines (the layout
        // of the scene pipeline needs the set from here), so they are resolved
        // right here; the bloom will read exactly the same thing again.
        {
            VkPhysicalDeviceProperties tsProps{};
            vkGetPhysicalDeviceProperties(m_gpu.physicalDevice(), &tsProps);
            m_timestampPeriod     = tsProps.limits.timestampPeriod;
            m_timestampsSupported = tsProps.limits.timestampComputeAndGraphics && m_timestampPeriod > 0.0f;
        }
        m_fpPass.createPipelines(fpCtx());
        createPipeline();
        m_shadowPass.createResources(shadowCtx());
        m_skinningPass.createPipelines(skinningCtx());
        createSkinnedGraphicsPipelines();
        // BEFORE createDescriptorSets: each object's sets already write the
        // views of the two IBL cubemaps (bindings 5 and 6). Here they are created with
        // neutral content; initSkybox will fill them in if there is an environment.
        m_iblPass.createResources(iblCtx());
        // BEFORE createOffscreenImages: that is where the mip chain is created, and it
        // needs the bloom's descriptor set layout and pool already set up.
        createBloomPipelines();
        // Game UI: same pass and same samples as the composition, which is
        // where its batches are recorded (LDR, already tonemapped, on top of the scene).
        // Own pass and ONE sample: the UI no longer goes inside the composition,
        // so AA does not touch it nor does it depend on the number of samples of the
        // scene (and that is also why its pipeline does not need recreating when MSAA changes).
        m_uiBatch.init(m_gpu, m_res, m_uiRenderPass, VK_SAMPLE_COUNT_1_BIT);
        // WORLD canvases do not go in that pass: they are recorded inside the
        // SCENE one, with the camera's perspective and hidden by the geometry.
        // Their two variants are compiled against m_offscreenRenderPass and with ITS
        // samples, which are not those of the UI pass. They have to be redone every time
        // that renderpass is recreated; see recreateMsaaDependentPipelines().
        m_uiBatch.initWorldPipelines(m_gpu, m_offscreenRenderPass, m_aaSampleCount);
        // BEFORE createOffscreenImages (which calls createSsaoImages) and AFTER
        // ShadowPass::createResources: the depth pre-pass pipeline
        // reuses the shadow pass's pipeline layout, which is created there.
        m_depthPrepass.createRenderPassAndPipeline(depthPrepassCtx());
        m_ssaoPass.createPipelines(ssaoCtx());
        // After SSAO: it shares its depth sampler (DepthPrepassPass) and
        // its depth pre-pass, and the query pool relies on the
        // m_timestampsSupported that the bloom resolved.
        m_ssrPass.createPipelines(ssrCtx());
        // After SSR: it feeds from the SAME depth pre-pass and the same depth
        // sampler, and its query pool relies on the same
        // m_timestampsSupported.
        m_fogPass.createPipelines(fogCtx());
        // After the fog, and BEFORE createOffscreenImages: its images and
        // its descriptor sets are created with the swapchain and need the layout and
        // the pool already set up.
        m_motionBlurPass.createPipeline(motionBlurCtx());
        // BEFORE createOffscreenImages (which calls createAaImages): that is where the
        // AA descriptor sets are allocated, and they need their layouts and their
        // pools already set up. The query pool relies on the
        // m_timestampsSupported that the bloom resolved.
        m_aaPass.createPipelines(aaCtx());
        createAaQueryPools();
        // The UI layer was already initialized in initPresentation. In the editor,
        // createOffscreenImages needs it to be (it calls
        // registerUiTexture); in headless it does not call it, so the order does not
        // matter. Since initUi ran before (phase 1) and this call runs in
        // phase 2, the UI -> offscreen order is preserved as in the original
        // init.
        createOffscreenImages();

        // The uploads go in BATCHES, not one by one. Without a batch, the CmdScope of
        // each createBuffer/createImage does endOneTimeCommands on exit, that is,
        // submit and WAIT for the GPU: measured in a scene of 1000 meshes, 10,970
        // waits that cost 1,886 ms of the ~2,600 that all this took. That is
        // 73 %.
        //
        // TransferBatch exists precisely for that and only the asynchronous load
        // (addStaticMesh) used it; this path, the startup one, was left out. The
        // correction holds because the barriers still order things inside the
        // command buffer just as they ordered them between submits: what disappears is
        // the wait, not the synchronization.
        //
        // In batches and not a single batch for the whole scene because the staging
        // lives until the fence signals (TransferBatch::addStaging): a single
        // batch would keep the intermediate copies of ALL the scene's textures
        // alive at once. The batch size bounds that peak; raising it
        // saves waits and costs memory.
        constexpr size_t kMallasPorLote = 32;

        m_objects.resize(meshes.size());
        for(size_t i = 0; i < meshes.size(); i++)
        {
            if (!m_pendingBatch)
                m_pendingBatch = std::make_unique<TransferBatch>(m_gpu);

            buildRenderObject(meshes[i], m_objects[i], m_pendingBatch.get());

            if ((i + 1) % kMallasPorLote == 0)
                flushUploadsAndWait();
        }
        // The last batch, which almost never comes out round. And it is also what leaves
        // this path SYNCHRONOUS as it always was: on returning from here everything is
        // uploaded and in its layout, which is what the createDescriptorSets below
        // and whoever calls initSceneResources take for granted.
        flushUploadsAndWait();

        createUniformBuffers();
        createDescriptorPool();
        createDescriptorSets();
        // AFTER createUniformBuffers: the fog set references the frame's
        // UBO, and during init that buffer did not exist yet when
        // createOffscreenImages ran. On swapchain recreations it already exists and
        // createOffscreenImages redoes the sets.
        m_fogPass.createSets(fogCtx());
    }

    bool Renderer::beginSplash(const std::string& logoPath)
    {
        // On top of the swapchain render pass already created by initPresentation
        // (createRenderPass): color-only, a single attachment (VK_FORMAT =
        // m_swapChainFormat, no depth); see the comment "color only,
        // used by the UI pass" in createFramebuffers. The splash pipeline
        // (Task 3) is created with pDepthStencilState = nullptr, which is
        // compatible with this render pass precisely because it has no
        // depth/stencil attachment. It does not throw if the logo is missing.
        return m_splash.init(m_gpu, m_renderPass, m_swapChainFormat, logoPath);
    }

    void Renderer::drawSplashFrame(float alpha)
    {
        if (!m_splash.isInitialized()) return;

        vkWaitForFences(m_gpu.device(), 1, &m_inFlight[m_currentFrame], VK_TRUE, UINT64_MAX);

        uint32_t imageIndex;
        VkResult res = vkAcquireNextImageKHR(m_gpu.device(), m_swapChain, UINT64_MAX,
            m_imageAvailable[m_currentFrame], VK_NULL_HANDLE, &imageIndex);
        if (res == VK_ERROR_OUT_OF_DATE_KHR) return; // during the splash we do not recreate: the next frame will
        if (res != VK_SUCCESS && res != VK_SUBOPTIMAL_KHR) return;

        vkResetFences(m_gpu.device(), 1, &m_inFlight[m_currentFrame]);
        vkResetCommandBuffer(m_commandBuffers[m_currentFrame], 0);

        VkCommandBufferBeginInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        vkBeginCommandBuffer(m_commandBuffers[m_currentFrame], &bi);

        VkClearValue clear{};
        clear.color = { { 0.05f, 0.05f, 0.06f, 1.0f } }; // same background as the shader

        VkRenderPassBeginInfo rp{};
        rp.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rp.renderPass        = m_renderPass;
        rp.framebuffer       = m_swapChainFramebuffers[imageIndex];
        rp.renderArea.extent = m_swapChainExtent;
        rp.clearValueCount   = 1; // m_renderPass es color-only (createRenderPass, 1 attachment)
        rp.pClearValues      = &clear;
        vkCmdBeginRenderPass(m_commandBuffers[m_currentFrame], &rp, VK_SUBPASS_CONTENTS_INLINE);

        // Dynamic viewport/scissor (the pipeline declares them dynamic).
        VkViewport vp{ 0, 0, (float)m_swapChainExtent.width, (float)m_swapChainExtent.height, 0.0f, 1.0f };
        VkRect2D sc{ { 0, 0 }, m_swapChainExtent };
        vkCmdSetViewport(m_commandBuffers[m_currentFrame], 0, 1, &vp);
        vkCmdSetScissor(m_commandBuffers[m_currentFrame], 0, 1, &sc);

        float aspect = m_swapChainExtent.height > 0
            ? (float)m_swapChainExtent.width / (float)m_swapChainExtent.height : 1.0f;
        m_splash.recordDraw(m_commandBuffers[m_currentFrame], alpha, aspect);

        vkCmdEndRenderPass(m_commandBuffers[m_currentFrame]);
        vkEndCommandBuffer(m_commandBuffers[m_currentFrame]);

        VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo si{};
        si.sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.waitSemaphoreCount   = 1;
        si.pWaitSemaphores      = &m_imageAvailable[m_currentFrame];
        si.pWaitDstStageMask    = &waitStage;
        si.commandBufferCount   = 1;
        si.pCommandBuffers      = &m_commandBuffers[m_currentFrame];
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores    = &m_renderFinished[imageIndex];
        vkQueueSubmit(m_gpu.graphicsQueue(), 1, &si, m_inFlight[m_currentFrame]);

        VkPresentInfoKHR pi{};
        pi.sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        pi.waitSemaphoreCount = 1;
        pi.pWaitSemaphores    = &m_renderFinished[imageIndex];
        pi.swapchainCount     = 1;
        pi.pSwapchains        = &m_swapChain;
        pi.pImageIndices      = &imageIndex;
        vkQueuePresentKHR(m_gpu.presentQueue(), &pi);

        m_currentFrame = (m_currentFrame + 1) % MAX_FRAMES;
    }

    void Renderer::drawFrame(Window& window)
    {
        // 1. Wait for the previous frame to finish
        vkWaitForFences(m_gpu.device(), 1, &m_inFlight[m_currentFrame], VK_TRUE, UINT64_MAX);

        // 2. Request the next swapchain image
        uint32_t imageIndex;
        VkResult result;

        result = vkAcquireNextImageKHR(m_gpu.device(), m_swapChain, UINT64_MAX, m_imageAvailable[m_currentFrame], VK_NULL_HANDLE, &imageIndex);
        if(result == VK_ERROR_OUT_OF_DATE_KHR)
        {
            recreateSwapChain(window);
            // This frame never gets to record/draw commands: discarding here
            // avoids the gizmo vertices accumulated by drawX(...) before
            // this call being carried over, duplicated, to the next frame that does
            // draw. It is the rare case (throwing the work away without consuming it), and
            // that is why clear is not called: in the normal path, Gizmos::draw already empties.
            Gizmos::discard();
            return;
        }

        if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
        {
            throw std::runtime_error("failed to acquire next image!");
        }        

        vkResetFences(m_gpu.device(), 1, &m_inFlight[m_currentFrame]);

        // 3. Record the command buffer
        if(vkResetCommandBuffer(m_commandBuffers[m_currentFrame], 0) != VK_SUCCESS)
        {
            throw std::runtime_error("failed to reset command buffer!");
        }

        // A change of AA mode, of its resource parameters or of the viewport
        // panel size is resolved HERE: this frame's fence already
        // signaled and the command buffer has not been recorded yet, so this is the point
        // where images and pipelines can be destroyed without catching in-flight
        // work. And BEFORE buildUiFrame, which is the important part: the
        // reconstruction re-registers the viewport texture and gives it a
        // new VkDescriptorSet. If it ran afterwards, ImGui would already have recorded the
        // old one (just destroyed) in this frame's draw list.
        if (m_aaResourcesDirty) rebuildAaResources();
        // Same place and same reason: between frames and with the GPU stopped.
        if (m_shadowResourcesDirty) rebuildShadowResources();

        // Reflection probes: SAME place and same reason as the line above.
        // Here we can wait for the GPU to become free to bake a probe or
        // rewrite bindings 5/6 of a descriptor set. With no probes in the
        // scene it exits through the fast path without touching anything, and with probes already
        // baked and still it does not record a single command either: the per-frame
        // GPU cost is identical in all three cases.
        m_probePass.sync(probeCtx());

        // ── Build the UI frame (before recording the command buffer) ──────────────
        // In headless there is no UI layer to feed: the runtime blits
        // the offscreen image directly to the swapchain (see recordCommandBuffer).
        if (!m_headless && m_ui)
            m_ui->buildUiFrame(m_offscreenDescSet[m_currentFrame], m_sceneRoot, m_viewMatrix);

        // It is sampled HERE, after buildUiFrame() and not before: that draw()
        // is what can flip m_isPlaying (Play/Stop buttons) or mutate the
        // scene (Add/Remove/Create Camera). currentFrameCamera() reads both
        // things, so if it were called before draw(), the UBO (lit geometry)
        // and the command buffer recorded right below (skybox +
        // gizmos) could end up reading different cameras on the exact frame
        // of the click: a frame of visible tearing. The UBO is in
        // host-mapped memory: it is enough to write it before the vkQueueSubmit further
        // down, it does not need to be the first thing in the frame.
        //
        // The cascades are fitted to the camera frustum, so they are computed
        // here, with the same, now stable camera, and BEFORE the two that
        // consume them: updateUniformBuffer (which copies them into the UBO) and
        // recordCommandBuffer (which culls and records the shadow pass with them).
        // Forward+: it is frozen HERE, after buildUiFrame (which is what may
        // have changed the mode with the combo) and before the two that
        // consume it. Without this single point, the parameter block that
        // pbr.frag reads and the dispatch that gets recorded could come out of different modes on
        // the exact frame of the click.
        m_fpActiveMode = m_fpMode;

        // The frame's camera is sampled here and not inside the pass: it is the same
        // currentFrameCamera() that the shadow pass culling sees.
        {
            // The scene center, where a point light aims. It is taken
            // HERE, next to the cascades, because it is the same value the fog has to
            // see (via fogCtx) in this frame.
            SceneCenter centro;
            // sharedIndex < 0 = freed entry (or not yet uploaded): it has no
            // place in the world and would pull the average towards the origin.
            for (const RenderObject& object : m_objects)
                if (object.sharedIndex >= 0) centro.add(object.transform);
            for (const SkinnedRenderObject& character : m_skinnedObjects)
                centro.add(character.transform);
            if (!centro.get(m_sceneCenter))
                m_sceneCenter = glm::vec3(0.0f);

            const FrameCamera cascadeCam = currentFrameCamera();
            m_shadowPass.computeCascades(cascadeCam.view, cascadeCam.proj, m_lights,
                                         shadowDistance(), cascadeLambda(), m_sceneCenter);
        }
        updateUniformBuffer(m_currentFrame);

        recordCommandBuffer(imageIndex);

        // 4. Submit to the GPU
        // In headless pass 2 does not draw UI: it blits the offscreen image to the
        // swapchain (recordCommandBuffer), so the first real use of the
        // acquired image happens in TRANSFER, not in COLOR_ATTACHMENT_OUTPUT.
        // Waiting on the semaphore in TRANSFER too makes that ordering
        // local and explicit, instead of depending on the blit's barrier
        // chaining with earlier pass 1 work (see the long comment on
        // srcStageMask in recordCommandBuffer, which is still valid and explains
        // why THAT barrier cannot wait on TRANSFER alone). Adding a stage
        // to the wait can only make the GPU wait more, never less: it
        // changes nothing on the editor path.
        VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        if (m_headless) waitStage |= VK_PIPELINE_STAGE_TRANSFER_BIT;
        VkSubmitInfo submitInfo{};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.waitSemaphoreCount       = 1;
        submitInfo.pWaitSemaphores          = &m_imageAvailable[m_currentFrame];
        submitInfo.pWaitDstStageMask        = &waitStage;
        submitInfo.commandBufferCount       = 1;
        submitInfo.pCommandBuffers          = &m_commandBuffers[m_currentFrame];
        submitInfo.signalSemaphoreCount     = 1;
        submitInfo.pSignalSemaphores        = &m_renderFinished[imageIndex];
        if(vkQueueSubmit(m_gpu.graphicsQueue(), 1, &submitInfo, m_inFlight[m_currentFrame]) != VK_SUCCESS)
        {
            throw std::runtime_error("failed to submit graphics queue!");
        }

        // 5. Present
        VkPresentInfoKHR presentInfo{};
        presentInfo.sType               = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        presentInfo.waitSemaphoreCount  = 1;
        presentInfo.pWaitSemaphores     = &m_renderFinished[imageIndex];
        presentInfo.swapchainCount      = 1;
        presentInfo.pSwapchains         = &m_swapChain;
        presentInfo.pImageIndices       = &imageIndex;
        result = vkQueuePresentKHR(m_gpu.presentQueue(), &presentInfo);
        if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR || m_framebufferResized) {
            m_framebufferResized = false;
            recreateSwapChain(window);
        } else if (result != VK_SUCCESS) {
            throw std::runtime_error("failed to present!");
        }

        // The gizmo vertices were already emptied by Gizmos::draw when consuming them, so
        // the next cycle of drawX(...), which the caller invokes BEFORE
        // calling drawFrame, starts from an empty buffer without anyone having
        // to remember anything.
        m_currentFrame = (m_currentFrame + 1) % MAX_FRAMES;
    }

    void Renderer::shutdown()
    {
        if (m_gpu.device() == VK_NULL_HANDLE) return;
        vkDeviceWaitIdle(m_gpu.device());

        // The vkDeviceWaitIdle above is the precondition of flushAll: without it,
        // this would destroy resources the GPU may still be reading.
        m_deferredDeletes.flushAll(m_gpu.device());

        // The upload batches, NOW and not as members after m_gpu.shutdown():
        // an exception halfway through a load (e.g. VRAM exhausted in addSkinnedMesh)
        // leaves m_pendingBatch open, and its destructor frees its command buffer
        // and its staging with the device. Destroyed after the device, that was a
        // vkFreeCommandBuffers on an invalid device and the loader aborted the
        // process (0xC0000409) before the host's catch said anything. After
        // the WaitIdle above, the in-flight ones have already finished.
        m_inFlightBatches.clear();
        m_pendingBatch.reset();

        destroyOffscreenImages();
        if (!m_headless && m_ui) m_ui->shutdownUi();
        vkDestroyRenderPass(m_gpu.device(), m_offscreenRenderPass, nullptr);
        m_offscreenRenderPass = VK_NULL_HANDLE;
        // Bloom + composition. The images and sets are already gone with
        // destroyOffscreenImages (it calls destroyBloomImages); what remains here are the
        // objects that do not depend on the swapchain size.
        vkDestroyPipeline(m_gpu.device(), m_compositePipeline, nullptr);
        vkDestroyPipelineLayout(m_gpu.device(), m_compositePipelineLayout, nullptr);
        vkDestroyDescriptorPool(m_gpu.device(), m_compositeDescPool, nullptr);
        vkDestroyDescriptorSetLayout(m_gpu.device(), m_compositeDescLayout, nullptr);
        vkDestroyRenderPass(m_gpu.device(), m_compositeRenderPass, nullptr);
        m_compositeRenderPass = VK_NULL_HANDLE;
        // The UI one does not depend on the number of samples, so it is only
        // destroyed here, in the real teardown.
        if (m_uiRenderPass != VK_NULL_HANDLE)
        {
            vkDestroyRenderPass(m_gpu.device(), m_uiRenderPass, nullptr);
            m_uiRenderPass = VK_NULL_HANDLE;
        }
        m_bloomPass.destroyPipelines(bloomCtx());
        // SSAO. The images, views, framebuffers and sets went away with
        // destroyOffscreenImages (it calls destroySsaoImages); what remains here is what does
        // not depend on the size. The pipeline layout is the shadow pass's and
        // is destroyed with it, further down.
        m_ssaoPass.destroyPipelines(ssaoCtx());
        m_depthPrepass.destroyRenderPassAndPipeline(depthPrepassCtx());

        // SSR: the images and sets are already gone with destroyOffscreenImages;
        // only what is independent of the size remains here.
        m_ssrPass.destroyPipelines(ssrCtx());

        // Volumetric fog: it has no image or sampler of its own (it writes inside
        // the HDR and samples with the SSAO sampler and the shadow map one), so
        // everything of its is here except the sets.
        m_fogPass.destroyPipelines(fogCtx());

        // Motion blur: here what does not depend on the size. The images and
        // sets went away with destroyImages.
        m_motionBlurPass.destroyPipeline(motionBlurCtx());

        // Forward+: the grid and the index list went away with
        // destroyOffscreenImages; what remains here is what does not depend on the size. The
        // three persistently mapped buffers do not need unmap: the mapping dies
        // with the memory, same as in the UBO and in the instance SSBO.
        m_fpPass.destroyPipelines(fpCtx());

        // Anti-aliasing: the images, framebuffers and sets went away with
        // destroyOffscreenImages (it calls destroyAaImages); what remains here is what does not
        // depend on the size or the mode.
        m_aaPass.destroyPipelinesAndRenderPasses(aaCtx());
        if (m_perfQueryPool != VK_NULL_HANDLE)
        {
            vkDestroyQueryPool(m_gpu.device(), m_perfQueryPool, nullptr);
            m_perfQueryPool = VK_NULL_HANDLE;
        }
        if (m_aaQueryPool != VK_NULL_HANDLE)
        {
            vkDestroyQueryPool(m_gpu.device(), m_aaQueryPool, nullptr);
            m_aaQueryPool = VK_NULL_HANDLE;
        }

        for(auto sem : m_renderFinished){
            vkDestroySemaphore(m_gpu.device(), sem, nullptr);
        }

        for(int i = 0; i < MAX_FRAMES; i++)
        {            
            vkDestroySemaphore(m_gpu.device(), m_imageAvailable[i], nullptr);
            vkDestroyFence(m_gpu.device(), m_inFlight[i], nullptr);
        }
        for(auto framebuffer : m_swapChainFramebuffers)
        {
            vkDestroyFramebuffer(m_gpu.device(), framebuffer, nullptr);
        }
        vkDestroyPipeline(m_gpu.device(), m_pipeline, nullptr);
        vkDestroyPipeline(m_gpu.device(), m_wireframePipeline, nullptr);
        // The FOUR outline ones, static and skinned: they belong to this pass, not
        // to the Renderer. Before releasing the pipeline layout, which is the one the
        // Context lends it.
        m_outlinePass.destroyResources(outlineCtx());
        vkDestroyPipelineLayout(m_gpu.device(), m_pipelineLayout, nullptr);
        vkDestroyRenderPass(m_gpu.device(), m_renderPass, nullptr);
        for(VkImageView imageView : m_swapChainImageViews)
        {
            vkDestroyImageView(m_gpu.device(), imageView, nullptr);
        }                        
        vkDestroySwapchainKHR(m_gpu.device(), m_swapChain, nullptr);
        // The m_descriptorPools chain is destroyed further down, AFTER the two loops that
        // call destroyRenderObject and destroySkinnedRenderObject: both
        // functions free sets from the pool (created with
        // FREE_DESCRIPTOR_SET_BIT to support rebuildSkinnedMesh), and destroying
        // the pool here first would leave an already destroyed handle to free from.
        m_objects.clear();
        m_staticSlots.clear();
        // No refcounts or deferral: the vkDeviceWaitIdle above guarantees that
        // nobody is reading, and by now there is nobody left to draw.
        m_sharedMeshes.destroyAll([this](const SharedGpuMesh& gpu) {
            destroySharedGpuMesh(gpu);
        });
        for(int i = 0; i < MAX_FRAMES; i++)
        {
            vkDestroyBuffer(m_gpu.device(), m_uniformBuffers[i], nullptr);
            vkFreeMemory(m_gpu.device(), m_uniformBuffersMemory[i], nullptr);
        }
        vkDestroyDescriptorSetLayout(m_gpu.device(), m_descriptorSetLayout, nullptr);
        // Instance SSBO: buffers, pool, sets and layout. One line, like the
        // thirteen passes next to it; before it was four and you had to remember
        // all four.
        m_instanceBuffers.destroy(instanceCtx());
        vkDestroyImageView(m_gpu.device(), m_depthImageView, nullptr);
        vkDestroyImage(m_gpu.device(), m_depthImage, nullptr);
        vkFreeMemory(m_gpu.device(), m_depthImageMemory, nullptr);
        // The sampler shared by ALL the material textures. Here and not in
        // destroySharedGpuMesh: there it is borrowed, and destroying it with the first
        // material would leave the others pointing at a dead sampler.
        m_res.destroySharedSampler();
        // Shadow map. The Context carries the two set layouts that were already
        // destroyed four lines above; destroyResources does not touch them (a
        // pipeline layout outlives the set layouts it was created with).
        m_shadowPass.destroyResources(shadowCtx());
        // The probes BEFORE the global IBL: the probes' destroy() does not touch the
        // convolution pipelines, but if the order were reversed a future cleanup
        // path with a pending convolution would be left without them.
        m_probePass.destroy(probeCtx());
        m_iblPass.destroyResources(iblCtx());
        vkDestroyPipeline(m_gpu.device(), m_skinnedGfxPipeline, nullptr);
        vkDestroyPipeline(m_gpu.device(), m_skinnedWireframePipeline, nullptr);
        for (auto& obj : m_skinnedObjects)
        {
            destroySkinnedRenderObject(obj);
        }

        m_skinnedObjects.clear();
        m_skinnedSlots.clear();
        // The three fill-ins shared by the meshes without a material, AFTER
        // the characters: they are the last one that releases material textures, and
        // releasing them earlier left it destroying already dead handles (H79).
        // All the characters were released above: the cache of their textures
        // must be empty. If not, someone skipped the release; a warning is given and
        // nothing is destroyed blindly (same criterion as H79).
        if (m_skinnedTextures.size() != 0)
            fprintf(stderr, "[Renderer] %zu character textures not released at shutdown\n",
                    m_skinnedTextures.size());
        if (m_skinnedGeometry.size() != 0)
            fprintf(stderr, "[Renderer] %zu character geometries not released at shutdown\n",
                    m_skinnedGeometry.size());
        m_res.destroySharedPlaceholders();
        // Now yes: there is no pending destroySkinnedRenderObject left that
        // needs to free sets from the pool chain.
        for (VkDescriptorPool pool : m_descriptorPools)
        {
            if (pool != VK_NULL_HANDLE)
                vkDestroyDescriptorPool(m_gpu.device(), pool, nullptr);
        }
        m_descriptorPools.clear();
        // Same as the one above: there is no pending destroySkinnedRenderObject left
        // that needs to free sets from the compute pool.
        m_skinningPass.destroyPipelines(skinningCtx());
        m_skybox.shutdown(m_gpu);
        m_splash.shutdown(m_gpu);
        Gizmos::shutdown(m_gpu);
        // The atlases BEFORE the batch: their descriptor sets come from their pool, and
        // destroying the pool first would leave the handles dangling.
        for (auto& atlas : m_uiAtlases) atlas->destroy(m_gpu);
        m_uiAtlases.clear();
        // Pointers to what was just destroyed: out before anyone
        // can request them again.
        m_uiAtlasByPath.clear();
        m_uiAtlasImGuiId.clear();
        // The thumbnail atlas, with the other atlases and before the device dies.
        destroyThumbAtlas();
        for (auto& font : m_uiFonts) font->destroy(m_gpu);
        m_uiFonts.clear();
        m_uiBatch.shutdown(m_gpu);
        printf("destroy render items OK\n"); fflush(stdout);
        m_gpu.shutdown();
    }

    UiTextureAtlas* Renderer::loadUiAtlas(const std::string& path)
    {
        // The same path twice is the same atlas: the editor queries it to
        // show its sprites and the sync asks for it every time a widget changes.
        if (auto it = m_uiAtlasByPath.find(path); it != m_uiAtlasByPath.end())
            return it->second;

        auto atlas = std::make_unique<UiTextureAtlas>();
        if (!atlas->loadFromFile(m_gpu, m_res, path)) return nullptr;
        // Sub-rects, if there are any: without a sidecar the atlas is used whole, which is what
        // it always did. It is not an error for it to be missing.
        atlas->loadSprites(UiTextureAtlas::spriteSheetPathFor(path));
        if (!m_uiBatch.registerAtlas(m_gpu, *atlas))
        {
            atlas->destroy(m_gpu);
            return nullptr;
        }
        m_uiAtlases.push_back(std::move(atlas));
        m_uiAtlasByPath[path] = m_uiAtlases.back().get();
        return m_uiAtlases.back().get();
    }

    uint64_t Renderer::uiAtlasTextureId(const UiTextureAtlas* atlas)
    {
        // Without an editor there is nobody to register it with, and without a view nothing has
        // been uploaded.
        if (!atlas || !m_ui || atlas->view() == VK_NULL_HANDLE) return 0;

        if (auto it = m_uiAtlasImGuiId.find(atlas); it != m_uiAtlasImGuiId.end())
            return it->second;

        const uint64_t id = m_ui->registerUiTexture((uint64_t)m_uiBatch.sampler(),
                                                    (uint64_t)atlas->view());
        m_uiAtlasImGuiId[atlas] = id;
        return id;
    }

    namespace
    {
        // The editor swapchain is B8G8R8A8_SRGB: sampling does sRGB -> linear, and the
        // write encodes again. Identity, so the thumbnails come out with
        // the image's colors. (D3D12 uses another one because of its UNORM RTV.)
        constexpr VkFormat kThumbFormat = VK_FORMAT_R8G8B8A8_SRGB;
    }

    uint64_t Renderer::uiThumbnailAtlasId()
    {
        if (m_thumbImGuiId != 0) return m_thumbImGuiId;
        if (m_thumbFailed || !m_ui) return 0;

        try
        {
            m_res.createBlankImage(kThumbAtlasSize, kThumbAtlasSize, kThumbFormat,
                                   m_thumbImage, m_thumbMemory);
            m_res.createTextureImageView(m_thumbImage, m_thumbView, kThumbFormat);
        }
        catch (const std::exception&)
        {
            destroyThumbAtlas();
            m_thumbFailed = true;
            return 0;
        }
        m_thumbImGuiId = m_ui->registerUiTexture((uint64_t)m_uiBatch.sampler(),
                                                 (uint64_t)m_thumbView);
        return m_thumbImGuiId;
    }

    bool Renderer::uploadUiThumbnails(const ThumbnailTile* tiles, size_t count)
    {
        if (m_thumbImGuiId == 0 || !tiles || count == 0) return false;

        std::vector<ImageTileUpload> uploads;
        uploads.reserve(count);
        for (size_t i = 0; i < count; ++i)
        {
            if (tiles[i].slot >= kThumbSlotCount || !tiles[i].rgba) return false;
            ImageTileUpload u;
            u.x    = (tiles[i].slot % kThumbAtlasCells) * kThumbCell;
            u.y    = (tiles[i].slot / kThumbAtlasCells) * kThumbCell;
            u.w    = kThumbCell;
            u.h    = kThumbCell;
            u.rgba = tiles[i].rgba;
            uploads.push_back(u);
        }
        try
        {
            m_res.uploadPixelsToImageRegions(m_thumbImage, uploads.data(), uploads.size());
        }
        catch (const std::exception&)
        {
            return false;
        }
        return true;
    }

    void Renderer::destroyThumbAtlas()
    {
        const VkDevice device = m_gpu.device();
        if (m_thumbView   != VK_NULL_HANDLE) vkDestroyImageView(device, m_thumbView, nullptr);
        if (m_thumbImage  != VK_NULL_HANDLE) vkDestroyImage(device, m_thumbImage, nullptr);
        if (m_thumbMemory != VK_NULL_HANDLE) vkFreeMemory(device, m_thumbMemory, nullptr);
        m_thumbView    = VK_NULL_HANDLE;
        m_thumbImage   = VK_NULL_HANDLE;
        m_thumbMemory  = VK_NULL_HANDLE;
        m_thumbImGuiId = 0;
    }

    UiFont* Renderer::loadUiFont(const std::string& path, float bakePx)
    {
        auto font = std::make_unique<UiFont>();
        if (!font->loadFromFile(m_gpu, m_res, path, bakePx)) return nullptr;
        // The font CONTAINS its atlas, so the descriptor registration is
        // exactly the same as for a sprite atlas.
        if (!m_uiBatch.registerAtlas(m_gpu, font->atlas()))
        {
            font->destroy(m_gpu);
            return nullptr;
        }
        m_uiFonts.push_back(std::move(font));
        return m_uiFonts.back().get();
    }

    void Renderer::syncUiCanvases(const std::vector<UiCanvasBinding>& bindings)
    {
        // Matches by ownerId: the slots that survive keep their tree and
        // their cache, so reordering the canvases in the hierarchy does not rebuild
        // what has not changed (that would show up as flicker).
        matchUiCanvasSlots(bindings, m_uiSlots);

        for (size_t i = 0; i < bindings.size(); i++)
        {
            UiCanvasSlot& s = *m_uiSlots[i];
            const UiCanvasBinding& b = bindings[i];
            if (b.canvas) b.canvas->applyTo(s.canvas);
            const UiCanvasRenderMode modo =
                b.canvas ? b.canvas->renderMode : UiCanvasRenderMode::ScreenSpace;
            // Changing mode puts the canvas in or takes it out of the input distribution
            // (screenCanvasesTopFirst filters by ScreenSpace). If it goes to World
            // mid-press (renderMode is writable from Lua) it never sees the
            // MouseUp and keeps its capture and its hover: on coming back it would steal
            // the pointer from the one on top for a frame. releaseInput releases it.
            if (modo != s.mode) s.canvas.releaseInput();
            s.mode      = modo;
            s.depthTest = b.canvas ? b.canvas->depthTest  : true;
            // Copy by value of the world settings and of the GameObject's
            // transform: the model matrix is computed at RECORD time (it needs the
            // camera view for the billboard) and by then the binding
            // no longer exists. Without a canvas the default component is kept, which
            // never actually gets read because the mode will be ScreenSpace.
            if (b.canvas) s.component = *b.canvas;
            s.worldTransform = b.worldTransform;
            syncUiWidgets(b.widgets, s.canvas, s.cache, *this);
        }
    }

    UiCanvas& Renderer::uiCanvas()
    {
        // The FIRST screen canvas, in scene order: it is the same
        // criterion the temporary shim (Task 4) used, so a project with a
        // single screen canvas looks exactly the same as before.
        for (auto& s : m_uiSlots)
            if (s && s->mode == UiCanvasRenderMode::ScreenSpace) return s->canvas;
        return m_uiCanvasFallback;
    }

    const UiCanvas& Renderer::uiCanvas() const
    {
        for (const auto& s : m_uiSlots)
            if (s && s->mode == UiCanvasRenderMode::ScreenSpace) return s->canvas;
        return m_uiCanvasFallback;
    }

    void Renderer::screenUiCanvases(std::vector<UiCanvas*>& out)
    {
        // The order comes from the SAME free function that D3D12 uses: the topmost
        // first, that is, the UI pass (which walks m_uiSlots in order)
        // in reverse. Duplicating the criterion here is how the two backends
        // get out of sync.
        screenCanvasesTopFirst(m_uiSlots, out);
    }

    const UiCanvas* Renderer::uiCanvasOf(uint64_t ownerId) const
    {
        // Same free function as D3D12: a single lookup criterion.
        return findCanvasByOwner(m_uiSlots, ownerId);
    }

    const UiElement* Renderer::findUiNode(const std::string& name) const
    {
        // ALL the canvases, not just the screen one: a button of a world
        // canvas also has to be able to carry a gizmo in the editor.
        for (const auto& s : m_uiSlots)
        {
            if (!s) continue;
            if (const UiElement* n = findUiNodeIn(s->canvas.root(), name)) return n;
        }
        return nullptr;
    }

    void Renderer::initSkybox(const std::array<std::string, 6>& facePaths)
    {
        // kHdrFormat: the skybox draws in the scene pass, which since the bloom
        // comes out in float. Its color goes through the composition tonemap like
        // the rest of the scene, and that is what lets the sky generate bloom.
        m_skybox.init(m_gpu, m_offscreenRenderPass, kHdrFormat, facePaths, m_aaSampleCount);
        // The just-loaded cubemap is the IBL source. Only once, here: it is
        // the point that both the editor and DonTopoRuntime go through, and it does not
        // depend on anything from the editor.
        m_iblPass.precompute(iblCtx());
    }

    void Renderer::setCamera(const Camera& camera)
    {
        m_viewMatrix = camera.getViewMatrix();
        m_camera = camera;
    }

    Renderer::FrameCamera Renderer::currentFrameCamera() const
    {
        const float aspect = viewportAspect();

        // Edit: the usual projection (fixed 45° + near/far derived from
        // m_cameraDistance). Not touched on purpose: the component only rules
        // in Play, so the editor's look does not change.
        FrameCamera fc{ m_viewMatrix,
                        glm::perspective(glm::radians(45.0f), aspect,
                                          m_cameraDistance * 0.001f, m_cameraDistance * 3.0f),
                        m_camera.getPos() };
        fc.proj[1][1] *= -1.0f; // Vulkan Y flip

        // Play with a camera in the scene: the CameraComponent rules. m_camera and
        // m_viewMatrix are NEVER touched; they remain the editor's, so
        // when Play stops the view comes back on its own, without saving or restoring state
        // (and without main.cpp, which calls setCamera every frame, noticing).
        // Without a camera in the scene it falls back to the case above; the warning to the Log is
        // given by EditorUI when starting Play, not here (this runs every frame).
        if (isPlaying() && m_scene)
        {
            if (GameObject* cam = m_scene->findCamera())
            {
                const auto& c = cam->getCameraComponent();
                fc.view = CameraComponent::viewFromWorld(cam->worldTransform);
                fc.proj = c->projectionMatrix(aspect);
                fc.eye  = glm::vec3(cam->worldTransform[3]);
            }
        }
        return fc;
    }

    // The three delegate: the culling geometry lives in Renderer/Frustum.h and
    // Renderer/SkinnedBounds.h, outside this file and outside Vulkan. The
    // wrappers stay because they are the door through which the tests and the
    // rest of this file come in.
    Renderer::Frustum Renderer::frustumFromViewProj(const glm::mat4& m)
    {
        return Culling::frustumFromViewProj(m);
    }

    bool Renderer::aabbVisible(const Frustum& frustum,
                               const glm::vec3& localMin,
                               const glm::vec3& localMax,
                               const glm::mat4& model)
    {
        return Culling::aabbVisible(frustum, localMin, localMax, model);
    }

    float Renderer::skinnedBoundRadius(const SkinnedMesh& mesh)
    {
        return Culling::skinnedBoundRadius(mesh);
    }

    void Renderer::createSwapChain(Window& window)
    {
        VkSurfaceCapabilitiesKHR surfaceCapabilities;
        if(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(m_gpu.physicalDevice(), m_gpu.surface(), &surfaceCapabilities) != VK_SUCCESS) {
            throw std::runtime_error("failed to get surface capabilities!");
        }

        uint32_t formatCount;
        if(vkGetPhysicalDeviceSurfaceFormatsKHR(m_gpu.physicalDevice(), m_gpu.surface(), &formatCount, nullptr) != VK_SUCCESS) {
            throw std::runtime_error("failed to get surface formats!");
        }

        // A driver that says it supports the surface but gives not even one format
        // leaves the vector empty, and the choice further down starts by reading
        // surfaceFormats[0]: an out-of-range access at startup, with no
        // diagnostic. It is a case that should not happen, and that is exactly why it has to be
        // reported instead of reading garbage.
        if (formatCount == 0) {
            throw std::runtime_error("surface reports zero formats!");
        }

        std::vector<VkSurfaceFormatKHR> surfaceFormats(formatCount);
        if(vkGetPhysicalDeviceSurfaceFormatsKHR(m_gpu.physicalDevice(), m_gpu.surface(), &formatCount, surfaceFormats.data()) != VK_SUCCESS) {
            throw std::runtime_error("failed to get surface formats!");
        }

        VkSurfaceFormatKHR chosenFormat = surfaceFormats[0];
        for(auto& surfaceFormat : surfaceFormats)
        {
            if(surfaceFormat.format == VK_FORMAT_B8G8R8A8_SRGB && surfaceFormat.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
                m_swapChainFormat = surfaceFormat.format;
                chosenFormat = surfaceFormat;
                break;
            }
        }
        
        m_swapChainFormat = chosenFormat.format;
        m_swapChainColorSpace = chosenFormat.colorSpace;

        VkExtent2D extent;
        if(surfaceCapabilities.currentExtent.width != UINT32_MAX)
        {
            extent = surfaceCapabilities.currentExtent;
        }
        else
        {
            int width, height;
            glfwGetFramebufferSize(window.getNativeWindow(), &width, &height);
            
            extent.width = std::clamp((uint32_t)width, surfaceCapabilities.minImageExtent.width, surfaceCapabilities.maxImageExtent.width);
            extent.height = std::clamp((uint32_t)height, surfaceCapabilities.minImageExtent.height, surfaceCapabilities.maxImageExtent.height);            
        }
        
        m_swapChainExtent = extent;
        // The internal render size comes from this one: the same except in SSAA, where
        // it is this one times the factor. It has to be set BEFORE creating the depth
        // and the intermediate targets, which already use the internal resolution.
        updateRenderExtent();

        uint32_t imageCount = surfaceCapabilities.minImageCount + 1;
        if(surfaceCapabilities.maxImageCount > 0 && imageCount > surfaceCapabilities.maxImageCount)
        {
            imageCount = surfaceCapabilities.maxImageCount;
        }

        VkSwapchainCreateInfoKHR createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
        createInfo.surface = m_gpu.surface();
        createInfo.minImageCount = imageCount;
        createInfo.imageFormat = m_swapChainFormat;
        createInfo.imageColorSpace = m_swapChainColorSpace;
        createInfo.imageExtent = m_swapChainExtent;
        createInfo.imageArrayLayers = 1;
        // TRANSFER_DST: in headless mode the offscreen image is blitted here instead
        // of the UI being drawn on top. The flag is unconditional so that
        // editor and runtime share the same resource creation path.
        createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        createInfo.preTransform = surfaceCapabilities.currentTransform;
        createInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        // Presentation mode. FIFO (vsync) is the only one the spec guarantees,
        // so it is the default and the destination of any fallback. The others are
        // queried from the device and cached here, which is where we already have surface
        // and physicalDevice; presentModeSupported() reads that cache.
        {
            uint32_t n = 0;
            vkGetPhysicalDeviceSurfacePresentModesKHR(m_gpu.physicalDevice(), m_gpu.surface(), &n, nullptr);
            std::vector<VkPresentModeKHR> disponibles(n);
            if (n) vkGetPhysicalDeviceSurfacePresentModesKHR(m_gpu.physicalDevice(), m_gpu.surface(),
                                                             &n, disponibles.data());
            m_mailboxDisponible   = false;
            m_immediateDisponible = false;
            for (VkPresentModeKHR m : disponibles)
            {
                if (m == VK_PRESENT_MODE_MAILBOX_KHR)    m_mailboxDisponible   = true;
                if (m == VK_PRESENT_MODE_IMMEDIATE_KHR)  m_immediateDisponible = true;
            }
        }

        VkPresentModeKHR modo = VK_PRESENT_MODE_FIFO_KHR;
        if (presentMode() == PresentMode::Mailbox && m_mailboxDisponible)
            modo = VK_PRESENT_MODE_MAILBOX_KHR;
        else if (presentMode() == PresentMode::Immediate && m_immediateDisponible)
            modo = VK_PRESENT_MODE_IMMEDIATE_KHR;
        createInfo.presentMode = modo;
        createInfo.clipped = VK_TRUE;

        if(vkCreateSwapchainKHR(m_gpu.device(), &createInfo, nullptr, &m_swapChain) != VK_SUCCESS)
        {
            throw std::runtime_error("failed to create swap chain!");
        }
        
        if(vkGetSwapchainImagesKHR(m_gpu.device(), m_swapChain, &imageCount, nullptr) != VK_SUCCESS)
        {
            throw std::runtime_error("failed to get swap chain images!");
        }
        m_swapChainImages.resize(imageCount);
        if(vkGetSwapchainImagesKHR(m_gpu.device(), m_swapChain, &imageCount, m_swapChainImages.data()) != VK_SUCCESS)
        {
            throw std::runtime_error("failed to get swap chain images!");
        }
        printf("SwapChain OK\n"); fflush(stdout);
    }

    void Renderer::createImageViews()
    {
        m_swapChainImageViews.resize((m_swapChainImages.size()));

        for(size_t i = 0; i < m_swapChainImages.size(); i++)
        {
            VkImageViewCreateInfo createInfo{};
            createInfo.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            createInfo.image    = m_swapChainImages[i];
            createInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
            createInfo.format   = m_swapChainFormat;
            // Channel mapping (identity = no changes)
            createInfo.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
            createInfo.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
            createInfo.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
            createInfo.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
            // Which part of the image we use
            createInfo.subresourceRange.aspectMask      = VK_IMAGE_ASPECT_COLOR_BIT;
            createInfo.subresourceRange.baseMipLevel    = 0;
            createInfo.subresourceRange.levelCount      = 1;
            createInfo.subresourceRange.baseArrayLayer  = 0;
            createInfo.subresourceRange.layerCount      = 1;

            if(vkCreateImageView(m_gpu.device(), &createInfo, nullptr, &m_swapChainImageViews[i]) != VK_SUCCESS)
            {
                throw std::runtime_error("failed to create image views!");
            }
        }

        printf("Image View OK\n"); fflush(stdout);
    }

    // 3D scene pass → offscreen (finalLayout=SHADER_READ so the UI can sample it)
    void Renderer::createOffscreenRenderPass()
    {
        VkAttachmentDescription colorAtt{};
        // HDR and not m_swapChainFormat: here the scene comes out WITHOUT tonemapping (see the
        // end of pbr.frag), so the attachment has to hold values
        // above 1.0 or the bloom threshold would find nothing.
        colorAtt.format         = kHdrFormat;
        colorAtt.samples        = m_aaSampleCount;
        colorAtt.loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
        // With MSAA what is kept is the RESOLVED one, not the multisample: nobody
        // reads this attachment afterwards, so storing it would be paying the
        // bandwidth of N samples to throw them away.
        colorAtt.storeOp        = (m_aaSampleCount == VK_SAMPLE_COUNT_1_BIT)
                                ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE;
        colorAtt.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        colorAtt.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        colorAtt.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
        colorAtt.finalLayout    = (m_aaSampleCount == VK_SAMPLE_COUNT_1_BIT)
                                ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        VkAttachmentReference colorRef{};
        colorRef.attachment = 0;
        colorRef.layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        // Resolve target: m_hdrImage, the usual ONE-sample image.
        // It comes out in SHADER_READ_ONLY just like without MSAA, so SSAO, SSR,
        // the bloom and the composition read exactly what they used to read.
        VkAttachmentDescription resolveAtt = colorAtt;
        resolveAtt.samples      = VK_SAMPLE_COUNT_1_BIT;
        resolveAtt.loadOp       = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        resolveAtt.storeOp      = VK_ATTACHMENT_STORE_OP_STORE;
        resolveAtt.finalLayout  = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkAttachmentReference resolveRef{};
        resolveRef.attachment = 2;
        resolveRef.layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        VkAttachmentDescription depthAtt{};
        depthAtt.format         = VK_FORMAT_D32_SFLOAT;
        depthAtt.samples        = m_aaSampleCount;
        depthAtt.loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
        // STORE and no longer DONT_CARE: the composition pass loads this same
        // depth so that the selection outline and the gizmos still have
        // something to test against after having moved there.
        depthAtt.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
        depthAtt.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        depthAtt.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depthAtt.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
        depthAtt.finalLayout    = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

        VkAttachmentReference depthRef{};
        depthRef.attachment = 1;
        depthRef.layout     = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount    = 1;
        subpass.pColorAttachments       = &colorRef;
        subpass.pDepthStencilAttachment = &depthRef;
        // Without MSAA there is nothing to resolve and the pointer stays null, which is
        // how this pass has been until now.
        if (m_aaSampleCount != VK_SAMPLE_COUNT_1_BIT)
            subpass.pResolveAttachments = &resolveRef;

        // Dependencies: they guarantee that the bloom (compute) and the composition
        // (fragment) can read the texture when the pass ends.
        VkSubpassDependency deps[2]{};
        deps[0].srcSubpass    = VK_SUBPASS_EXTERNAL;
        deps[0].dstSubpass    = 0;
        // COMPUTE also in the src: the reader of the HDR image is no longer just the
        // composition fragment shader, but also the bloom downsample of the
        // previous frame, and this dependency is what prevents overwriting it.
        //
        // And the fragment tests of the PREVIOUS frame: m_depthImage is a single
        // image for the two in-flight frames, and this pass debuts it every frame
        // from UNDEFINED with a clear. Without waiting for its depth writes
        // (this pass and the composition, which loads it), the transition and the clear
        // of one frame could overlap with the depth the other was still writing
        // (WRITE_AFTER_WRITE from synchronization validation).
        deps[0].srcStageMask  = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                                VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        deps[0].dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                                VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        deps[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        deps[0].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

        deps[1].srcSubpass    = 0;
        deps[1].dstSubpass    = VK_SUBPASS_EXTERNAL;
        // The depth is written in LATE_FRAGMENT_TESTS and is now read by the composition
        // pass, so it goes into srcStageMask along with the color.
        deps[1].srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        deps[1].dstStageMask  = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT
                              | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        deps[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
        deps[1].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

        VkAttachmentDescription attachments[] = { colorAtt, depthAtt, resolveAtt };
        VkRenderPassCreateInfo rpInfo{};
        rpInfo.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        rpInfo.attachmentCount = (m_aaSampleCount == VK_SAMPLE_COUNT_1_BIT) ? 2 : 3;
        rpInfo.pAttachments    = attachments;
        rpInfo.subpassCount    = 1;
        rpInfo.pSubpasses      = &subpass;
        rpInfo.dependencyCount = 2;
        rpInfo.pDependencies   = deps;

        if (vkCreateRenderPass(m_gpu.device(), &rpInfo, nullptr, &m_offscreenRenderPass) != VK_SUCCESS)
            throw std::runtime_error("failed to create offscreen render pass!");

        printf("offscreen render pass OK\n"); fflush(stdout);
    }

    void Renderer::createUiRenderPass()
    {
        // A single attachment: the final LDR image, with whatever is already inside.
        // ONE sample ALWAYS, even if the scene runs with MSAA: there is no longer any
        // 3D geometry to smooth here, and the AA itself has already resolved.
        VkAttachmentDescription colorAtt{};
        colorAtt.format         = m_swapChainFormat;
        colorAtt.samples        = VK_SAMPLE_COUNT_1_BIT;
        // LOAD and not DONT_CARE: under the UI is the entire scene.
        colorAtt.loadOp         = VK_ATTACHMENT_LOAD_OP_LOAD;
        colorAtt.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
        colorAtt.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        colorAtt.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        // Enters and leaves in SHADER_READ_ONLY: that is how the two paths that
        // write before leave it (composition without AA, or the resolve pass) and
        // how the editor panel and the headless blit expect it.
        colorAtt.initialLayout  = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        colorAtt.finalLayout    = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkAttachmentReference colorRef{};
        colorRef.attachment = 0;
        colorRef.layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments    = &colorRef;

        // What came before wrote color (composition or AA resolve); this
        // writes over the same thing again, so the dependency goes from color output
        // to color output.
        VkSubpassDependency dep{};
        dep.srcSubpass    = VK_SUBPASS_EXTERNAL;
        dep.dstSubpass    = 0;
        dep.srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        dep.dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

        VkRenderPassCreateInfo rpInfo{};
        rpInfo.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        rpInfo.attachmentCount = 1;
        rpInfo.pAttachments    = &colorAtt;
        rpInfo.subpassCount    = 1;
        rpInfo.pSubpasses      = &subpass;
        rpInfo.dependencyCount = 1;
        rpInfo.pDependencies   = &dep;

        if (vkCreateRenderPass(m_gpu.device(), &rpInfo, nullptr, &m_uiRenderPass) != VK_SUCCESS)
            throw std::runtime_error("failed to create ui render pass!");
    }

    void Renderer::createCompositeRenderPass()
    {
        // Color: the usual LDR offscreen image (swapchain format), the
        // one the UI samples and the one the headless runtime blits. The
        // full-screen triangle covers it entirely, so there is no need to load or
        // clear anything beforehand.
        VkAttachmentDescription colorAtt{};
        colorAtt.format         = m_swapChainFormat;
        colorAtt.samples        = m_aaSampleCount;
        colorAtt.loadOp         = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        // With MSAA what is kept is the resolved one, same as in the scene pass:
        // nobody reads the multisample color.
        colorAtt.storeOp        = (m_aaSampleCount == VK_SAMPLE_COUNT_1_BIT)
                                ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE;
        colorAtt.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        colorAtt.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        colorAtt.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
        colorAtt.finalLayout    = (m_aaSampleCount == VK_SAMPLE_COUNT_1_BIT)
                                ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        VkAttachmentReference colorRef{};
        colorRef.attachment = 0;
        colorRef.layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        // MSAA resolve target: m_offscreenImage. This pass being
        // multisample too is what makes the selection outline and the
        // gizmos come out smoothed: they are rasterized at N samples against the scene's
        // multisample depth. Resolving the depth to draw them in a
        // one-sample pass is not an option, VK_KHR_depth_stencil_resolve is Vulkan 1.2.
        VkAttachmentDescription resolveAtt = colorAtt;
        resolveAtt.samples      = VK_SAMPLE_COUNT_1_BIT;
        resolveAtt.loadOp       = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        resolveAtt.storeOp      = VK_ATTACHMENT_STORE_OP_STORE;
        resolveAtt.finalLayout  = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkAttachmentReference resolveRef{};
        resolveRef.attachment = 2;
        resolveRef.layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        // Depth: the SAME buffer the scene pass just wrote, loaded
        // as is. It is what allows the outline and the gizmos to be drawn
        // here, already in LDR, without the tonemap touching their color and respecting
        // exactly the scene's depth.
        VkAttachmentDescription depthAtt{};
        depthAtt.format         = VK_FORMAT_D32_SFLOAT;
        depthAtt.samples        = m_aaSampleCount;
        depthAtt.loadOp         = VK_ATTACHMENT_LOAD_OP_LOAD;
        depthAtt.storeOp        = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depthAtt.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        depthAtt.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depthAtt.initialLayout  = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        depthAtt.finalLayout    = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

        VkAttachmentReference depthRef{};
        depthRef.attachment = 1;
        depthRef.layout     = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount    = 1;
        subpass.pColorAttachments       = &colorRef;
        subpass.pDepthStencilAttachment = &depthRef;
        if (m_aaSampleCount != VK_SAMPLE_COUNT_1_BIT)
            subpass.pResolveAttachments = &resolveRef;

        VkSubpassDependency deps[2]{};
        // Input: waits for the scene pass (color+depth) and for the bloom
        // dispatches, which are the two sources this pass samples.
        deps[0].srcSubpass    = VK_SUBPASS_EXTERNAL;
        deps[0].dstSubpass    = 0;
        deps[0].srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
                              | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT
                              | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT
                              | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        deps[0].dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
                              | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT
                              | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        deps[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
                              | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT
                              | VK_ACCESS_SHADER_WRITE_BIT
                              | VK_ACCESS_SHADER_READ_BIT;
        deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
                              | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT
                              | VK_ACCESS_SHADER_READ_BIT;

        // Output: the UI (or the headless blit) reads the already composited image.
        deps[1].srcSubpass    = 0;
        deps[1].dstSubpass    = VK_SUBPASS_EXTERNAL;
        deps[1].srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        deps[1].dstStageMask  = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        deps[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        deps[1].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

        VkAttachmentDescription attachments[] = { colorAtt, depthAtt, resolveAtt };
        VkRenderPassCreateInfo rpInfo{};
        rpInfo.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        rpInfo.attachmentCount = (m_aaSampleCount == VK_SAMPLE_COUNT_1_BIT) ? 2 : 3;
        rpInfo.pAttachments    = attachments;
        rpInfo.subpassCount    = 1;
        rpInfo.pSubpasses      = &subpass;
        rpInfo.dependencyCount = 2;
        rpInfo.pDependencies   = deps;

        if (vkCreateRenderPass(m_gpu.device(), &rpInfo, nullptr, &m_compositeRenderPass) != VK_SUCCESS)
            throw std::runtime_error("failed to create composite render pass!");

        printf("composite render pass OK\n"); fflush(stdout);
    }

    // UI pass → swapchain (color only, no depth)
    void Renderer::createRenderPass()
    {
        VkAttachmentDescription colorAtt{};
        colorAtt.format         = m_swapChainFormat;
        colorAtt.samples        = VK_SAMPLE_COUNT_1_BIT;
        colorAtt.loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
        colorAtt.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
        colorAtt.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        colorAtt.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        colorAtt.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
        colorAtt.finalLayout    = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

        VkAttachmentReference colorRef{};
        colorRef.attachment = 0;
        colorRef.layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments    = &colorRef;

        VkSubpassDependency dep{};
        dep.srcSubpass    = VK_SUBPASS_EXTERNAL;
        dep.dstSubpass    = 0;
        dep.srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep.srcAccessMask = 0;
        dep.dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

        VkRenderPassCreateInfo rpInfo{};
        rpInfo.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        rpInfo.attachmentCount = 1;
        rpInfo.pAttachments    = &colorAtt;
        rpInfo.subpassCount    = 1;
        rpInfo.pSubpasses      = &subpass;
        rpInfo.dependencyCount = 1;
        rpInfo.pDependencies   = &dep;

        if (vkCreateRenderPass(m_gpu.device(), &rpInfo, nullptr, &m_renderPass) != VK_SUCCESS)
            throw std::runtime_error("failed to create UI render pass!");

        printf("UI render pass OK\n"); fflush(stdout);
    }

    // Swapchain framebuffers: color only, used by the UI pass
    void Renderer::createFramebuffers()
    {
        m_swapChainFramebuffers.resize(m_swapChainImageViews.size());
        for (size_t i = 0; i < m_swapChainImageViews.size(); i++)
        {
            VkFramebufferCreateInfo fbInfo{};
            fbInfo.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            fbInfo.renderPass      = m_renderPass;
            fbInfo.attachmentCount = 1;
            fbInfo.pAttachments    = &m_swapChainImageViews[i];
            fbInfo.width           = m_swapChainExtent.width;
            fbInfo.height          = m_swapChainExtent.height;
            fbInfo.layers          = 1;

            if (vkCreateFramebuffer(m_gpu.device(), &fbInfo, nullptr, &m_swapChainFramebuffers[i]) != VK_SUCCESS)
                throw std::runtime_error("failed to create swapchain framebuffer!");
        }
        printf("swapchain framebuffers OK\n"); fflush(stdout);
    }



    void Renderer::createCommandBuffers()
    {
        m_commandBuffers.resize(2);
        VkCommandBufferAllocateInfo commandBufferAllocateInfo{};
        commandBufferAllocateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        commandBufferAllocateInfo.commandPool = m_gpu.commandPool();
        commandBufferAllocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        commandBufferAllocateInfo.commandBufferCount = 2;

        if(vkAllocateCommandBuffers(m_gpu.device(), &commandBufferAllocateInfo, m_commandBuffers.data()) != VK_SUCCESS)
        {
            throw std::runtime_error("failed to allocate command buffers!");
        }

        printf("command buffer allocate OK\n"); fflush(stdout);
    }

    void Renderer::createSyncObjects()
    {
        VkSemaphoreCreateInfo semaphoreInfo{};
        semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        
        VkFenceCreateInfo fenceCreateInfo{};
        fenceCreateInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fenceCreateInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;   // First frame

        for(int i = 0; i < MAX_FRAMES; i++)
        {
            if(vkCreateSemaphore(m_gpu.device(), &semaphoreInfo, nullptr, &m_imageAvailable[i]) != VK_SUCCESS                
                || vkCreateFence(m_gpu.device(), &fenceCreateInfo, nullptr, &m_inFlight[i]) != VK_SUCCESS)
            {
                // m_imageAvailable: signals that a swapchain image is available
                // m_renderFinished: signals that rendering finished, ready to present
                // m_inFlight: fence that blocks the CPU until the GPU finished that frame
                throw std::runtime_error("failed to create sync objects!");
            }
        }

        m_renderFinished.resize(m_swapChainImages.size());
        VkSemaphoreCreateInfo semInfo{};
        semInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        for (auto& sem : m_renderFinished)
        {
            if (vkCreateSemaphore(m_gpu.device(), &semInfo, nullptr, &sem) != VK_SUCCESS)
            {
                throw std::runtime_error("failed to create renderFinished semaphore!");
            }                
            printf("sync objects OK\n"); fflush(stdout);
        }            
    }

    void Renderer::recordSelectionOutline(VkCommandBuffer cmd, const Frustum& camFrustum)
    {
        if (!m_outlinePass.hasTarget())
            return;

        const int outlineStatic  = m_outlinePass.staticTarget();
        const int outlineSkinned = m_outlinePass.skinnedTarget();

        // Hull thickness relative to the object's size in world space: with a fixed
        // value, a large object would barely show a border and a small one would be
        // swallowed by it.

        if (outlineStatic >= 0 && (size_t)outlineStatic < m_objects.size())
        {
            const RenderObject&  obj = m_objects[outlineStatic];
            const SharedGpuMesh* gpu = m_sharedMeshes.get(obj.sharedIndex);
            // The same guards as the draw loop, literally: an object
            // with no outline because it is off camera (or hidden) is correct,
            // because the object was not drawn either.
            if (Visibility::objectVisible(obj, gpu, m_lastCompletedTicket, camFrustum))
            {
                const glm::vec3 extent = gpu->aabbMax - gpu->aabbMin;
                const float maxExtent  = glm::max(extent.x, glm::max(extent.y, extent.z));

                PushData push;
                push.transform = obj.transform;
                // flags.x = 0: the outline draws ONE instance with its matrix
                // here, not through the SSBO; outline.vert does not even read it.
                push.flags.y = outlineThickness(maxExtent, obj.transform);

                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    m_outlinePass.staticPipeline(isWireframeMode()));
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout,
                    0, 1, &gpu->descriptorSets[m_currentFrame], 0, nullptr);
                vkCmdPushConstants(cmd, m_pipelineLayout,
                    VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                    0, sizeof(PushData), &push);
                VkBuffer     vbs[]  = { gpu->vertexBuffer };
                VkDeviceSize offs[] = { 0 };
                vkCmdBindVertexBuffers(cmd, 0, 1, vbs, offs);
                vkCmdBindIndexBuffer(cmd, gpu->indexBuffer, 0, VK_INDEX_TYPE_UINT32);
                vkCmdDrawIndexed(cmd, gpu->indexCount, 1, 0, 0, 0);
            }
        }

        if (outlineSkinned >= 0 && (size_t)outlineSkinned < m_skinnedObjects.size())
        {
            // Visibility was already decided by the culling at the start of the frame:
            // it is the same one that governed the skinning compute, so if it is 0
            // the output buffer has not even been updated and drawing the
            // hull would show an old pose.
            // With the "Visible" checkbox off exactly the same thing happens: the
            // compute is not dispatched, so there is no pose to draw.
            if (m_skinnedVisible[outlineSkinned] &&
                m_skinnedObjects[outlineSkinned].meshVisible)
            {
                const SkinnedRenderObject& sobj = m_skinnedObjects[outlineSkinned];
                // matGfx empty: there would be nowhere to get set 0 from, and the UBO that
                // outline.vert reads lives there. Without materials there is no outline.
                if (sobj.outputVertexBuffer != VK_NULL_HANDLE && !sobj.matGfx.empty())
                {
                    PushData push;
                    push.transform = sobj.transform;
                    push.flags.y = outlineThickness(sobj.restMaxExtent, sobj.transform);

                    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                        m_outlinePass.skinnedPipeline(isWireframeMode()));
                    // A single draw over the whole index buffer: the submeshes only
                    // exist to change material, and the outline is a
                    // flat color.
                    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout,
                        0, 1, &sobj.matGfx[0].descSets[m_currentFrame], 0, nullptr);
                    vkCmdPushConstants(cmd, m_pipelineLayout,
                        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                        0, sizeof(PushData), &push);
                    VkBuffer     vbs[]  = { sobj.outputVertexBuffer };
                    VkDeviceSize offs[] = { 0 };
                    vkCmdBindVertexBuffers(cmd, 0, 1, vbs, offs);
                    vkCmdBindIndexBuffer(cmd, sobj.indexBuffer, 0, VK_INDEX_TYPE_UINT32);
                    vkCmdDrawIndexed(cmd, sobj.indexCount, 1, 0, 0, 0);
                }
            }
        }
    }

    void Renderer::setPerfCaptureEnabled(bool on)
    {
        if (on == m_perfCapture) return;
        m_perfCapture = on;
        // When turned off the pending slots are invalidated: their queries will not be
        // reset again, and reading them when the panel is reopened would return garbage from the
        // frame in which it was closed (or NOT_READY forever).
        for (int f = 0; f < MAX_FRAMES; f++) m_perfQueryPending[f] = false;
        if (!on)
        {
            m_shadowGpuMs   = 0.0f;
            m_sceneGpuMs    = 0.0f;
            m_statDrawCalls = 0;
            m_statInstances = 0;
            m_statCulled    = 0;
        }
    }

    void Renderer::recordScenePass(VkCommandBuffer cmd, const FrameCamera& fc,
                                       const Frustum& camFrustum, bool perfStamp)
    {
    // ── Pass 1: 3D scene → offscreen ──────────────────────────────────────────
    {
        VkClearValue clearValues[2];
        clearValues[0].color        = {0.0f, 0.0f, 0.0f, 1.0f};
        clearValues[1].depthStencil = {1.0f, 0};

        VkRenderPassBeginInfo rpInfo{};
        rpInfo.sType               = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rpInfo.renderPass          = m_offscreenRenderPass;
        rpInfo.framebuffer         = m_offscreenFramebuffer[m_currentFrame];
        rpInfo.renderArea.extent   = m_renderExtent;
        rpInfo.renderArea.offset   = {0, 0};
        rpInfo.clearValueCount     = 2;
        rpInfo.pClearValues        = clearValues;

        if (perfStamp)
        {
            vkCmdWriteTimestamp(m_commandBuffers[m_currentFrame], VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                m_perfQueryPool, m_currentFrame * 4 + 2);
        }
        vkCmdBeginRenderPass(m_commandBuffers[m_currentFrame], &rpInfo, VK_SUBPASS_CONTENTS_INLINE);

        VkViewport viewport{};
        viewport.width    = (float)m_renderExtent.width;
        viewport.height   = (float)m_renderExtent.height;
        viewport.minDepth = 0.0f;
        viewport.maxDepth = 1.0f;
        vkCmdSetViewport(m_commandBuffers[m_currentFrame], 0, 1, &viewport);

        VkRect2D scissor{};
        scissor.extent = m_renderExtent;
        vkCmdSetScissor(m_commandBuffers[m_currentFrame], 0, 1, &scissor);

        vkCmdBindPipeline(m_commandBuffers[m_currentFrame], VK_PIPELINE_BIND_POINT_GRAPHICS,
            isWireframeMode() ? m_wireframePipeline : m_pipeline);
        // The shadow pass and the depth pre-pass have already written their part of the
        // buffer: their transforms go first and the ones here after. colorPass =
        // true because this is the only one that paints color, and therefore the only
        // one that puts the SSR strength in the grouping key: it is a push
        // constant per group, just like metallic and roughness, so two
        // objects with the same mesh and different strength cannot go in the
        // same draw.
        gatherAndBatch(camFrustum, /*colorPass*/ true);

        // Counters for the Performance panel: they are counted over the same
        // candidates that buildInstanceBatches just grouped, so
        // they reflect exactly what is going to be drawn below.
        if (perfStamp)
        {
            for (const auto& cand : m_batchCandidates)
            {
                if (!cand.visible) m_statCulled++;
            }
            m_statDrawCalls += (int)m_instanceBatches.size();
            for (const InstanceBatch& b : m_instanceBatches)
            {
                m_statInstances += (int)b.instanceCount;
            }
        }

        // Set 1 only once for the whole pass: the SSBO does not change between
        // draws, and the skinned pipeline below shares the layout, so
        // it stays bound and valid for it too.
        const VkDescriptorSet instSet = m_instanceBuffers.set(m_currentFrame);
        vkCmdBindDescriptorSets(m_commandBuffers[m_currentFrame], VK_PIPELINE_BIND_POINT_GRAPHICS,
            m_pipelineLayout, 1, 1, &instSet, 0, nullptr);

        // Forward+: one per frame and common to ALL the pass's draws (static
        // and skinned), so it is bound only once here. It goes INSIDE the scene
        // pass and not before: the shadow pass and the depth pre-pass use
        // ShadowPass's pipeline layout, which only declares two sets, and
        // binding with it leaves set 2 undefined.
        const VkDescriptorSet fpSceneSet = m_fpPass.set(m_currentFrame);
        if (fpSceneSet != VK_NULL_HANDLE)
        {
            vkCmdBindDescriptorSets(m_commandBuffers[m_currentFrame], VK_PIPELINE_BIND_POINT_GRAPHICS,
                m_pipelineLayout, 2, 1, &fpSceneSet, 0, nullptr);
        }

        for (const InstanceBatch& batch : m_instanceBatches)
        {
            // It cannot be nullptr: only candidates that already passed the guard above
            // reach a group.
            const SharedGpuMesh* gpu = m_sharedMeshes.get(batch.sharedIndex);
            vkCmdBindDescriptorSets(m_commandBuffers[m_currentFrame], VK_PIPELINE_BIND_POINT_GRAPHICS,
                m_pipelineLayout, 0, 1, &gpu->descriptorSets[m_currentFrame], 0, nullptr);
            PushData push;
            // transform stays at identity: with useInstancing = 1 the
            // vertex shader takes the model matrix from the SSBO. metallic and roughness
            // come from the GROUP and no longer from the shared entry: since they are
            // per object, "same entry" does not imply "same factors"; what
            // guarantees that the value holds for all the draw's instances is
            // that both go into the grouping key (InstanceBatching.h).
            push.metallic  = batch.metallic;
            push.roughness = batch.roughness;
            push.flags.x   = 1.0f;
            // pbr.frag dumps it into the HDR alpha, which is the per-pixel mask
            // that ssr.comp reads.
            push.flags.y   = batch.ssrStrength;
            vkCmdPushConstants(m_commandBuffers[m_currentFrame], m_pipelineLayout,
                VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                0, sizeof(PushData), &push);
            VkBuffer vbs[]      = { gpu->vertexBuffer };
            VkDeviceSize offs[] = { 0 };
            vkCmdBindVertexBuffers(m_commandBuffers[m_currentFrame], 0, 1, vbs, offs);
            vkCmdBindIndexBuffer(m_commandBuffers[m_currentFrame], gpu->indexBuffer, 0, VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexed(m_commandBuffers[m_currentFrame], gpu->indexCount,
                batch.instanceCount, 0, 0, batch.firstInstance);
        }

        if (!m_skinnedObjects.empty())
        {
            vkCmdBindPipeline(m_commandBuffers[m_currentFrame], VK_PIPELINE_BIND_POINT_GRAPHICS,
                isWireframeMode() ? m_skinnedWireframePipeline : m_skinnedGfxPipeline);

            for (size_t si = 0; si < m_skinnedObjects.size(); si++)
            {
                // Deleted, in flight or off camera: the decision was already made by
                // the culling at the start of the frame, the same one that decided whether
                // its compute was dispatched.
                if (!m_skinnedVisible[si])
                {
                    if (perfStamp) m_statCulled++;
                    continue;
                }
                SkinnedRenderObject& sobj = m_skinnedObjects[si];
                // "Visible" checkbox of the Mesh component. It goes here and not in
                // m_skinnedVisible because that flag also governs the compute
                // dispatch (which keeps running: the selection outline reads its vertices)
                // and the outline itself.
                if (!sobj.meshVisible) continue;
                VkBuffer     vbs[]  = { sobj.outputVertexBuffer };
                VkDeviceSize offs[] = { 0 };
                vkCmdBindVertexBuffers(m_commandBuffers[m_currentFrame], 0, 1, vbs, offs);
                vkCmdBindIndexBuffer(m_commandBuffers[m_currentFrame],
                    sobj.indexBuffer, 0, VK_INDEX_TYPE_UINT32);

                for (auto& sm : sobj.subMeshes)
                {
                    SkinnedMatGfx& mgfx = sobj.matGfx[sm.materialIndex];
                    PushData push;
                    push.transform = sobj.transform;
                    push.metallic  = mgfx.metallic;
                    push.roughness = mgfx.roughness;
                    // flags.x stays at 0 (skinned path, own matrix).
                    push.flags.y   = m_ssrEnabled ? sobj.ssrStrength : 0.0f;
                    vkCmdBindDescriptorSets(m_commandBuffers[m_currentFrame],
                        VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout,
                        0, 1, &mgfx.descSets[m_currentFrame], 0, nullptr);
                    vkCmdPushConstants(m_commandBuffers[m_currentFrame], m_pipelineLayout,
                        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                        0, sizeof(PushData), &push);
                    vkCmdDrawIndexed(m_commandBuffers[m_currentFrame],
                        sm.indexCount, 1, sm.indexStart, 0, 0);
                    if (perfStamp) { m_statDrawCalls++; m_statInstances++; }
                }
            }
        }

        // Outline of the selected object: after all the geometry
        // (it needs the complete depth buffer so the hull only peeks out
        // at the edge) and before the skybox.
        // The selection outline and the gizmos are NO LONGER drawn here: they moved
        // to the composition pass, which is LDR. This pass comes out
        // untonemapped and would have changed their flat color.

        // Skybox projection (same pass, same camera as the culling
        // above). The Y-flip is already applied from currentFrameCamera().
        // With jitter when the mode is TAA: it has to move EXACTLY
        // like the geometry or the TAA would see a permanent edge between
        // the two. Outside TAA it is fc.proj as is.
        const glm::mat4 proj = m_aaPass.jitteredProj();

        // Skybox: fullscreen quad, depth LEQUAL with no write (at the end of the pass).
        // Omitted in wireframe: the background is already solid black (default clearValue).
        if (!isWireframeMode() && m_skybox.isInitialized()) {
            glm::mat4 rotView    = glm::mat4(glm::mat3(fc.view)); // no translation
            glm::mat4 invViewProj = glm::inverse(proj * rotView);
            m_skybox.draw(m_commandBuffers[m_currentFrame], invViewProj);
        }

        // ── WORLD canvases ───────────────────────────────────────────────
        // The last thing in the pass, behind the geometry and the skybox: they go with
        // alpha and have to blend over what is already there. Here (and not in the
        // UI pass) is what gives them perspective and what makes a
        // wall hide them: the scene depth buffer is loaded and the
        // variant with depthTest reads it (it does not write any).
        //
        // `proj` is the JITTERED one (the same as the skybox and the
        // geometry): with TAA, a world canvas without jitter would leave a
        // permanent edge against everything that does carry it.
        //
        // KNOWN LIMITATION: the post effects that reconstruct position from
        // depth treat the canvas as the GEOMETRY BEHIND IT.
        // A world canvas does NOT enter the depth pre-pass (that one only records
        // meshes) and does NOT write depth (depthWrite is off in the
        // three variants, on purpose: the UI goes with alpha). So in the
        // pixel it occupies, the depth the post effects read is that of what is
        // behind it. The three affected, all sampling the same depthTex
        // of the pre-pass:
        //   - fog.comp        -> a sign near the camera in front of a
        //                        distant wall gets the WALL'S fog:
        //                        it comes out over-fogged.
        //   - motion_blur.comp-> it receives the wall's motion
        //                        vectors: it drags when the camera moves.
        //   - taa.frag        -> it reprojects with the wall's depth.
        // It is not fixed here: putting them in the pre-pass would give them AO
        // occlusion and break the alpha. When verifying in the GUI, look at the
        // colors with the FOG OFF first, or the over-fogging gets confused
        // with a failure of the sRGB->linear conversion.
        //
        // The order is set by the free function in UiWidgetSync.h, which is the
        // one tested without a GPU (test_world_canvases_se_ordenan_de_lejos_a_cerca).
        // The draw data and the model matrix are already done above, before
        // beginFrame.
        sortWorldCanvasesBackToFront(m_uiSlots, fc.view, m_uiWorldOrder);
        for (UiCanvasSlot* s : m_uiWorldOrder)
        {
            if (s->drawData.empty()) continue;
            const glm::vec2 tam = s->canvas.referenceResolution;
            const glm::mat4 mvp = proj * fc.view * s->model;
            m_uiBatch.recordWorld(m_gpu, m_commandBuffers[m_currentFrame], s->drawData,
                                  mvp, s->depthTest,
                                  VkExtent2D{(uint32_t)tam.x, (uint32_t)tam.y},
                                  m_renderExtent, m_currentFrame);
        }

        vkCmdEndRenderPass(m_commandBuffers[m_currentFrame]);
        if (perfStamp)
        {
            vkCmdWriteTimestamp(m_commandBuffers[m_currentFrame], VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                                m_perfQueryPool, m_currentFrame * 4 + 3);
            // All four are written: the slot is now readable within two
            // frames. If the capture is turned off before, setPerfCaptureEnabled
            // clears the flag and an unreset pool is not read.
            m_perfQueryPending[m_currentFrame] = true;
        }
    }

    // SSR: it needs the scene color ALREADY lit, so it goes AFTER the
    // scene pass; and it adds the reflection inside the HDR itself BEFORE the bloom,
    // so that the reflection generates bloom and goes through the ACES tonemap like the
    // rest of the image. With the effect off it records nothing and the HDR
    // stays exactly as it came out of the render pass.
    m_ssrPass.record(ssrCtx(), m_commandBuffers[m_currentFrame], fc.proj);

    // Volumetric fog: after the SSR (it wants the color already lit and with the
    // reflections in it) and before the bloom, so that the in-scattering
    // blooms and goes through the ACES tonemap like the rest of the image.
    // Off, it records nothing.
    m_fogPass.record(fogCtx(), m_commandBuffers[m_currentFrame], fc.view, fc.proj);

    // Camera motion blur: after the fog (it blurs the image just as
    // it will be seen) and before the bloom, so that the trail drags the
    // highlights and blooms with them. Off, it records nothing.
    m_motionBlurPass.record(motionBlurCtx(), m_commandBuffers[m_currentFrame]);

    }

    void Renderer::recordBloomAndComposite(VkCommandBuffer cmd, const FrameCamera& fc,
                                               const Frustum& camFrustum, VkExtent2D uiExtent,
                                               uint32_t uiScreenVertices, uint32_t uiScreenIndices)
    {
    // ── Bloom + composition: HDR → tonemap → LDR offscreen ────────────────────
    {
        VkCommandBuffer cmd = m_commandBuffers[m_currentFrame];

        if (bloomEnabled())
        {
            m_bloomPass.beginQuery(bloomCtx(), cmd);
            m_bloomPass.record(bloomCtx(), cmd);
        }
        else
        {
            // Off: not a single dispatch of the chain, and no timestamps to measure.
            // The slot stops having a pending pair so that on turning it back on
            // a measurement from before the blackout is not read.
            m_bloomPass.skipQuery(bloomCtx());
            m_bloomPass.recordClear(bloomCtx(), cmd);
        }

        VkRenderPassBeginInfo rpInfo{};
        rpInfo.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rpInfo.renderPass        = m_compositeRenderPass;
        // With FXAA, SSAA or TAA the composition (tonemap + outline + gizmos)
        // goes to the intermediate image and the resolve pass takes it from there to
        // m_offscreenImage. In None and in MSAA it writes directly to
        // m_offscreenImage, exactly as before this feature: same
        // render pass, same commands.
        rpInfo.framebuffer       = needsAaIntermediate() ? m_aaPass.compositeFramebuffer(m_currentFrame)
                                                         : m_compositeFramebuffer[m_currentFrame];
        rpInfo.renderArea.extent = m_renderExtent;
        rpInfo.renderArea.offset = {0, 0};
        // Both attachments are DONT_CARE/LOAD: nothing to clear.
        rpInfo.clearValueCount   = 0;

        vkCmdBeginRenderPass(cmd, &rpInfo, VK_SUBPASS_CONTENTS_INLINE);

        VkViewport viewport{};
        viewport.width    = (float)m_renderExtent.width;
        viewport.height   = (float)m_renderExtent.height;
        viewport.minDepth = 0.0f;
        viewport.maxDepth = 1.0f;
        vkCmdSetViewport(cmd, 0, 1, &viewport);

        VkRect2D scissor{};
        scissor.extent = m_renderExtent;
        vkCmdSetScissor(cmd, 0, 1, &scissor);

        // Without a mip chain (tiny viewport) or with the effect off there is
        // nothing to add: the intensity is forced to 0 and only the
        // tonemap remains. The pass CANNOT be skipped: it is the one that tonemaps.
        const float intensity = (bloomEnabled() && m_bloomPass.mipCount() > 0) ? m_bloomIntensity : 0.0f;
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_compositePipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_compositePipelineLayout,
                                0, 1, &m_compositeSets[m_currentFrame], 0, nullptr);
        vkCmdPushConstants(cmd, m_compositePipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(float), &intensity);
        vkCmdDraw(cmd, 3, 1, 0, 0);

        // Outline and gizmos, already over the tonemapped image and with the
        // scene depth loaded: same result as when they lived
        // in the previous pass, but without going through the tonemap or the bloom.
        recordSelectionOutline(cmd, camFrustum);
        Gizmos::draw(cmd, fc.proj * fc.view, m_currentFrame);

        vkCmdEndRenderPass(cmd);

        // Only with the bloom on: the pair is opened above under the same
        // condition, and writing here without having reset would leave the query dirty.
        if (m_timestampsSupported && bloomEnabled())
            vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, m_bloomPass.queryPool(), m_currentFrame * 2 + 1);

        // Anti-aliasing: the last thing in the post chain, over LDR color already
        // tonemapped and with the selection outline and the gizmos already drawn
        // (so their edges are smoothed too, which is what is wanted: their
        // lines and the inverted hull are the most jagged thing on screen).
        m_aaPass.record(aaCtx(), cmd);

        // ── Game UI, already over the final image ─────────────────────────
        // After the AA on purpose: here the text is not smoothed by FXAA nor
        // dragged by the TAA history, and with SSAA it is drawn ONCE at the
        // output size instead of being supersampled. It is where the
        // DirectX 12 backend draws it too (over the resolved back buffer).
        //
        // It goes above the scene, the selection outline and the
        // gizmos, and below the editor interface, which is recorded in the
        // swapchain pass. With an empty canvas the pass is not even opened.
        //
        // The draw data of ALL the slots and the batch's beginFrame() are
        // already done ABOVE, before the scene pass: the world canvases
        // are recorded there and need the buffer sized and the cursors
        // reset before anyone else. Only the SCREEN ones remain here, which
        // are recorded one by one inside the SAME vkCmdBeginRenderPass.
        //
        // The opening condition is the SCREEN ones and only them: with the
        // frame total, a project with only world canvases
        // would open this pass to record not a single draw inside.
        if (uiScreenVertices > 0 && uiScreenIndices > 0 && m_uiFramebuffer[m_currentFrame] != VK_NULL_HANDLE)
        {
            VkRenderPassBeginInfo uiRp{};
            uiRp.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
            uiRp.renderPass        = m_uiRenderPass;
            uiRp.framebuffer       = m_uiFramebuffer[m_currentFrame];
            uiRp.renderArea.extent = uiExtent;
            uiRp.renderArea.offset = {0, 0};
            uiRp.clearValueCount   = 0;   // el attachment es LOAD

            vkCmdBeginRenderPass(cmd, &uiRp, VK_SUBPASS_CONTENTS_INLINE);

            // The viewport is dynamic state and this pass is its own: it has to be
            // set, it is not inherited from the composition one.
            VkViewport vp{};
            vp.width    = (float)uiExtent.width;
            vp.height   = (float)uiExtent.height;
            vp.minDepth = 0.0f;
            vp.maxDepth = 1.0f;
            vkCmdSetViewport(cmd, 0, 1, &vp);

            // There is NO beginFrame() here. The frame's was already called above,
            // before the scene pass, with the world + screen total.
            // Calling it again would reset the cursors to 0 and the screen
            // canvases would write OVER the vertices of the world ones,
            // which the GPU has not read yet (it reads the buffer when EXECUTING the
            // list, not when recording it): the world canvases would come out with the
            // screen one's geometry, without a single validation warning.

            // bottom=0 and top=height: (0,0) lands at the TOP left. It looks backwards
            // and it is exactly the opposite: in Vulkan NDC +Y goes DOWN, so
            // the OpenGL recipe (top=0, bottom=height) leaves [1][1] negative and
            // draws the WHOLE UI mirrored; invisible while there were only color
            // quads, obvious as soon as the first letter was drawn. RH_ZO because
            // Vulkan clips z outside [0,1] and a bare glm::ortho gives [-1,1].
            // From the CANVAS SPACE (output pixels), not the framebuffer: the
            // vertices arrive in those pixels and the viewport already stretches the NDC to the
            // whole framebuffer. With SSAA that leaves the UI supersampled instead of
            // shrunk to 1/factor, which is what came out when projecting with the render's
            // extent.
            const glm::mat4 uiProj = glm::orthoRH_ZO(0.0f, (float)uiExtent.width,
                                                     0.0f, (float)uiExtent.height,
                                                     0.0f, 1.0f);

            // Canvas and framebuffer are now the SAME space (output pixels):
            // the scissors are not scaled.
            for (auto& s : m_uiSlots)
            {
                if (!s || s->mode != UiCanvasRenderMode::ScreenSpace) continue;
                m_uiBatch.record(m_gpu, cmd, s->drawData, uiProj, uiExtent, uiExtent, m_currentFrame);
            }
            vkCmdEndRenderPass(cmd);
        }

        // End of the measurement of the complete render, now with the AA included. It is
        // the reference against which the overhead of SSAA and MSAA is compared,
        // which have no pass of their own to measure.
        if (m_timestampsSupported)
        {
            vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, m_aaQueryPool, m_currentFrame * 4 + 3);
            m_aaQueryPending[m_currentFrame] = true;
        }
    }

    }

    void Renderer::recordUiPass(VkCommandBuffer cmd, uint32_t imageIndex)
    {
    // ── Pass 2: UI → swapchain ────────────────────────────────────────────────
    if (!m_headless)
    {
        VkClearValue clearColor{};
        clearColor.color = {0.12f, 0.12f, 0.12f, 1.0f};

        VkRenderPassBeginInfo rpInfo{};
        rpInfo.sType               = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rpInfo.renderPass          = m_renderPass;
        rpInfo.framebuffer         = m_swapChainFramebuffers[imageIndex];
        rpInfo.renderArea.extent   = m_swapChainExtent;
        rpInfo.renderArea.offset   = {0, 0};
        rpInfo.clearValueCount     = 1;
        rpInfo.pClearValues        = &clearColor;

        vkCmdBeginRenderPass(m_commandBuffers[m_currentFrame], &rpInfo, VK_SUBPASS_CONTENTS_INLINE);
        if (m_ui) m_ui->recordUi(static_cast<void*>(m_commandBuffers[m_currentFrame]));
        vkCmdEndRenderPass(m_commandBuffers[m_currentFrame]);
    }
    else
    {
        // ── Pass 2 (headless): offscreen → swapchain ──────────────────────────
        // Without an editor there is nobody to sample the offscreen image, so it is
        // copied as is to the presentation image. It is a 1:1 blit: the
        // offscreen is created with the same format and extent as the swapchain
        // (createOffscreenImages). The offscreen renderpass declares
        // initialLayout=UNDEFINED, so there is no need to restore its layout
        // after the blit.
        VkCommandBuffer cmd = m_commandBuffers[m_currentFrame];
        const VkImageSubresourceRange range{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

        VkImageMemoryBarrier toTransferSrc{};
        toTransferSrc.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        toTransferSrc.oldLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        toTransferSrc.newLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        toTransferSrc.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toTransferSrc.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toTransferSrc.image               = m_offscreenImage[m_currentFrame];
        toTransferSrc.subresourceRange    = range;
        toTransferSrc.srcAccessMask       = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        toTransferSrc.dstAccessMask       = VK_ACCESS_TRANSFER_READ_BIT;

        VkImageMemoryBarrier toTransferDst{};
        toTransferDst.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        toTransferDst.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
        toTransferDst.newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toTransferDst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toTransferDst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toTransferDst.image               = m_swapChainImages[imageIndex];
        toTransferDst.subresourceRange    = range;
        toTransferDst.srcAccessMask       = 0;
        toTransferDst.dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;

        // srcStageMask = COLOR_ATTACHMENT_OUTPUT and not TRANSFER: this is not an
        // oversight, it is what orders this barrier (and therefore the blit below)
        // behind the vkAcquireNextImageKHR semaphore. This frame's submit
        // only waits on that semaphore at COLOR_ATTACHMENT_OUTPUT_BIT
        // (submitInfo.pWaitDstStageMask, further down), which does not cover TRANSFER, so
        // if the barrier waited only on TRANSFER, the driver would have no
        // dependency forcing it to come after the acquisition of the
        // swapchain image and the blit could run on an image that
        // is not yet ours (or overwrite the previous in-flight frame's). That it
        // works depends on Pass 1 (offscreen) ALWAYS emitting work at
        // COLOR_ATTACHMENT_OUTPUT before this barrier, so the srcStageMask
        // here chains with that work and ends up correctly after the
        // semaphore. If someday this srcStageMask gets "fixed" to
        // VK_PIPELINE_STAGE_TRANSFER_BIT (which is what seems obvious at
        // first sight), that chain breaks silently: without validation layers in
        // between there is no warning, just a blit occasionally on an image
        // not yet acquired.
        VkImageMemoryBarrier preBarriers[] = { toTransferSrc, toTransferDst };
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, nullptr, 0, nullptr, 2, preBarriers);

        VkImageBlit blit{};
        blit.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        blit.srcOffsets[0]  = { 0, 0, 0 };
        blit.srcOffsets[1]  = { (int32_t)effectiveViewport().width, (int32_t)effectiveViewport().height, 1 };
        blit.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        blit.dstOffsets[0]  = { 0, 0, 0 };
        blit.dstOffsets[1]  = { (int32_t)m_swapChainExtent.width, (int32_t)m_swapChainExtent.height, 1 };

        vkCmdBlitImage(cmd,
            m_offscreenImage[m_currentFrame], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            m_swapChainImages[imageIndex],     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            1, &blit, VK_FILTER_NEAREST);

        VkImageMemoryBarrier toPresent{};
        toPresent.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        toPresent.oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toPresent.newLayout           = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        toPresent.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toPresent.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toPresent.image               = m_swapChainImages[imageIndex];
        toPresent.subresourceRange    = range;
        toPresent.srcAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
        toPresent.dstAccessMask       = 0;

        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
            0, 0, nullptr, 0, nullptr, 1, &toPresent);
    }

    vkEndCommandBuffer(m_commandBuffers[m_currentFrame]);
    }

    void Renderer::recordCommandBuffer(uint32_t imageIndex)
    {
        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        if(vkBeginCommandBuffer(m_commandBuffers[m_currentFrame], &beginInfo) != VK_SUCCESS)
        {
            throw std::runtime_error("failed to begin command buffer!");
        }

        // Frame instance SSBO: it is sized BEFORE recording anything,
        // because growing it recreates the buffer and updates its descriptor set (here it
        // is safe: drawFrame already waited for this frame's fence). The worst case
        // is all objects being visible in ALL the passes: one per
        // shadow map cascade plus the scene one, hence the factor
        // SHADOW_CASCADES + 1. The cursor starts at 0: the cascades write
        // in front (one after another), the scene at the end.
        // +2 and not +1: besides the scene pass, the SSAO depth pre-pass
        // writes its own stretch of the SSBO with the camera's visible set.
        // Characters count the same as static ones: they are another draw with their
        // matrix in the same SSBO. Counting only the static ones left a
        // scene of pure characters with ZERO capacity, and the two passes that
        // walk them bailed out through a silent `break` (H23).
        // Ensuring the capacity and setting the cursor to 0 are the SAME operation and
        // that is why they go together: growing recreates the buffer, and a cursor that survives
        // that points at freed memory. Before they were two consecutive lines that
        // had to be remembered in that order.
        m_instanceBuffers.beginFrame(instanceCtx(), m_currentFrame,
                                     shadowInstanceCapacity((uint32_t)m_objects.size(),
                                                            (uint32_t)m_skinnedObjects.size()));
        m_statInstanceOverflow    = 0;

        // Frame camera: it is used by the skinned culling (down here), the
        // static one's and, later in the main pass, the skybox and the
        // gizmos. It is sampled ONCE so that all of them see exactly the same one.
        const FrameCamera fc = currentFrameCamera();
        const Frustum camFrustum = frustumFromViewProj(fc.proj * fc.view);

        // Visibility of the skinned ones BEFORE the compute: it is the same decision the
        // draw loop reads, so on the frame a character comes back
        // into camera its skinning is dispatched and it is drawn afterwards, in
        // this same command buffer, with its current animTime. Separating the two
        // decisions would be what leaves a frame with the old pose.
        //
        // The shadow pass only iterates m_objects, so the skinned ones do not
        // cast shadows and the camera frustum is enough.
        m_skinnedVisible.assign(m_skinnedObjects.size(), 0);
        for (size_t i = 0; i < m_skinnedObjects.size(); i++)
        {
            const SkinnedRenderObject& sobj = m_skinnedObjects[i];
            if (sobj.outputVertexBuffer == VK_NULL_HANDLE) continue; // deleted from the editor
            // In flight: its input SSBO and its textures were uploaded in a batch
            // whose fence has not signaled. No compute or draw until it completes.
            if (sobj.uploadTicket > m_lastCompletedTicket) continue;
            if (sobj.hasBounds &&
                !aabbVisible(camFrustum, glm::vec3(-sobj.boundRadius), glm::vec3(sobj.boundRadius),
                             sobj.transform))
            {
                continue;
            }
            m_skinnedVisible[i] = 1;
        }

        // ── Anti-aliasing measurement ─────────────────────────────────────────
        // Four queries per in-flight frame: [0,1] the mode's own pass (written
        // by recordAaPass) and [2,3] the complete render without UI, which is measured
        // ALWAYS, also without AA. The results read here are those from
        // two frames ago in this same slot: the fence already waited for them.
        if (m_timestampsSupported && m_aaQueryPending[m_currentFrame])
        {
            uint64_t total[2] = {};
            if (vkGetQueryPoolResults(m_gpu.device(), m_aaQueryPool, m_currentFrame * 4 + 2, 2,
                                      sizeof(total), total, sizeof(uint64_t),
                                      VK_QUERY_RESULT_64_BIT) == VK_SUCCESS)
            {
                m_renderGpuMs = (float)((double)(total[1] - total[0]) * m_timestampPeriod * 1e-6);
            }
            // The own pass's pair is read separately and only if that frame got to
            // write it: in None and in MSAA it does not exist, and asking for all four at
            // once would return NOT_READY for all of them and the total above would
            // be lost too.
            if (m_aaPass.passStamped(m_currentFrame))
            {
                uint64_t stamps[2] = {};
                if (vkGetQueryPoolResults(m_gpu.device(), m_aaQueryPool, m_currentFrame * 4, 2,
                                          sizeof(stamps), stamps, sizeof(uint64_t),
                                          VK_QUERY_RESULT_64_BIT) == VK_SUCCESS)
                {
                    m_aaGpuMs = (float)((double)(stamps[1] - stamps[0]) * m_timestampPeriod * 1e-6);
                }
            }
            if (++m_aaMeasuredFrames == 300)
            {
                static const char* kNames[] = { "none", "fxaa", "ssaa", "msaa", "taa" };
                printf("aa (%s, %ux samples): own pass %.3f ms, full render %.3f ms (%ux%u internal, %ux%u window)\n",
                       kNames[(int)m_aaActiveMode], (uint32_t)m_aaSampleCount, m_aaGpuMs, m_renderGpuMs,
                       m_renderExtent.width, m_renderExtent.height,
                       m_swapChainExtent.width, m_swapChainExtent.height);
                fflush(stdout);
            }
        }
        if (m_timestampsSupported)
        {
            // Single reset of the four: recordAaPass can no longer reset on its own
            // without clobbering the total's pair.
            vkCmdResetQueryPool(m_commandBuffers[m_currentFrame], m_aaQueryPool, m_currentFrame * 4, 4);
            vkCmdWriteTimestamp(m_commandBuffers[m_currentFrame], VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                m_aaQueryPool, m_currentFrame * 4 + 2);
        }

        // ── Performance panel measurement ────────────────────────────────────
        // Only if the panel is open. The read is of the slot from two
        // frames ago (this frame's fence already waited for it), without WAIT_BIT: if the
        // driver does not have them yet the previous value is kept and that is it.
        const bool perfStamp = m_timestampsSupported && m_perfCapture;
        if (perfStamp && m_perfQueryPending[m_currentFrame])
        {
            uint64_t st[4] = {};
            if (vkGetQueryPoolResults(m_gpu.device(), m_perfQueryPool, m_currentFrame * 4, 4,
                                      sizeof(st), st, sizeof(uint64_t),
                                      VK_QUERY_RESULT_64_BIT) == VK_SUCCESS)
            {
                m_shadowGpuMs = (float)((double)(st[1] - st[0]) * m_timestampPeriod * 1e-6);
                m_sceneGpuMs  = (float)((double)(st[3] - st[2]) * m_timestampPeriod * 1e-6);
            }
        }
        if (perfStamp)
        {
            vkCmdResetQueryPool(m_commandBuffers[m_currentFrame], m_perfQueryPool, m_currentFrame * 4, 4);
            m_statDrawCalls = 0;
            m_statInstances = 0;
            m_statCulled    = 0;
        }

        m_skinningPass.record(skinningCtx(), m_commandBuffers[m_currentFrame]);
        if (perfStamp)
        {
            vkCmdWriteTimestamp(m_commandBuffers[m_currentFrame], VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                m_perfQueryPool, m_currentFrame * 4);
        }
        recordShadowPass(m_commandBuffers[m_currentFrame]);
        if (perfStamp)
        {
            vkCmdWriteTimestamp(m_commandBuffers[m_currentFrame], VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                                m_perfQueryPool, m_currentFrame * 4 + 1);
        }
        // BEFORE the scene pass: pbr.frag needs the AO already resolved, and the AO
        // needs the depth of the WHOLE scene. That is why the depth pre-pass exists, and
        // why it goes here and not afterwards.
        recordSsaoPass(m_commandBuffers[m_currentFrame], camFrustum, fc.proj);
        // Forward+: AFTER the pre-pass (the tiled one reads that depth) and BEFORE
        // the scene pass, which is what consumes the light grid. In Off it
        // records not a single command.
        m_fpPass.record(fpCtx(), m_commandBuffers[m_currentFrame], fc.proj);

        // ── UI: draw data of ALL the frame's canvases, and beginFrame ────────────
        // Up here and not inside the UI pass, which is where it used to live. The reason
        // is that WORLD canvases are recorded in the SCENE pass, which starts
        // three lines below: if beginFrame() stayed in the UI pass, the
        // world ones would call record() BEFORE anyone had sized
        // the buffer or reset the cursors. The capacity would be that of the previous
        // frame (or 0) and the uiCursorFits guard would discard them SILENTLY:
        // not an error, nor a validation warning, nor a canvas on screen.
        //
        // And that is why the total has to be that of ALL the frame's canvases, world
        // and screen: a single shared buffer, a single beginFrame.
        // Calling it again for the UI pass would reset the cursors and the
        // screen canvases would overwrite the vertices of the world ones, which the GPU
        // has not read yet (it reads the buffer when EXECUTING, not when recording).
        //
        // Building the draw data here requires knowing each canvas's space
        // already, and it is known: the screen one's is effectiveViewport(), and the
        // world one's is its own referenceResolution (in World mode the canvas does not
        // fit any screen, CanvasComponent::applyTo sets it).
        const VkExtent2D uiExtent = effectiveViewport();
        uint32_t uiTotalVertices = 0;
        uint32_t uiTotalIndices  = 0;
        // The SCREEN ones apart: they are the only ones that open the UI pass. With the
        // frame total it would also open with only live world canvases, and
        // it would be a vkCmdBeginRenderPass with not a single draw inside.
        uint32_t uiScreenVertices = 0;
        uint32_t uiScreenIndices  = 0;
        for (auto& s : m_uiSlots)
        {
            if (!s) continue;
            if (s->mode == UiCanvasRenderMode::World)
            {
                // The MODEL matrix, here and not in syncUiCanvases: it needs the
                // camera view for the billboard, and syncUiCanvases does not have
                // it. Here it also lands BEFORE sortWorldCanvasesBackToFront,
                // which orders by reading the position from model[3]; computing it already
                // at record time would leave it at zero for the ordering.
                const glm::vec2 tam = s->canvas.referenceResolution;
                s->model = uiWorldCanvasMatrix(s->component, tam, s->worldTransform, fc.view);
                s->canvas.buildDrawData((uint32_t)tam.x, (uint32_t)tam.y, s->drawData);
            }
            else
            {
                s->canvas.buildDrawData(uiExtent.width, uiExtent.height, s->drawData);
                uiScreenVertices += (uint32_t)s->drawData.vertices.size();
                uiScreenIndices  += (uint32_t)s->drawData.indices.size();
            }
            uiTotalVertices += (uint32_t)s->drawData.vertices.size();
            uiTotalIndices  += (uint32_t)s->drawData.indices.size();
        }
        m_uiBatch.beginFrame(m_gpu, m_currentFrame, uiTotalVertices, uiTotalIndices);

        // Frame pass 1: the 3D scene to its own render target.
        recordScenePass(m_commandBuffers[m_currentFrame], fc, camFrustum, perfStamp);
        // Bloom and composition: from the scene's HDR to the LDR the screen will see.
        recordBloomAndComposite(m_commandBuffers[m_currentFrame], fc, camFrustum, uiExtent,
                                uiScreenVertices, uiScreenIndices);
        // Frame pass 2: the 2D UI over the swapchain image.
        recordUiPass(m_commandBuffers[m_currentFrame], imageIndex);
    }

    void Renderer::createPipeline()
    {
        auto vertCode = loadShaderFile("shaders/triangle.vert.spv");
        auto fragCode = loadShaderFile("shaders/pbr.frag.spv");

        VkShaderModule vertModule = createShaderModule(vertCode);
        VkShaderModule fragModule = createShaderModule(fragCode);

        // 1. Shader stages: which shader goes in each stage
        VkPipelineShaderStageCreateInfo vertStage{};
        vertStage.sType     = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        vertStage.stage     = VK_SHADER_STAGE_VERTEX_BIT;
        vertStage.module    = vertModule;
        vertStage.pName     = "main";

        VkPipelineShaderStageCreateInfo fragStage{};
        fragStage.sType     = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        fragStage.stage     = VK_SHADER_STAGE_FRAGMENT_BIT;
        fragStage.module    = fragModule;
        fragStage.pName     = "main";

        VkPipelineShaderStageCreateInfo stages[] = {vertStage,fragStage};

        // 2. Vertex input: no vertex buffer, the positions go in the shader
        VkVertexInputBindingDescription bindingDesc{};
        bindingDesc.binding     = 0;
        bindingDesc.stride      = sizeof(Vertex);
        bindingDesc.inputRate   = VK_VERTEX_INPUT_RATE_VERTEX;

        // Attribute 0: pos (vec2, offset 0)
        VkVertexInputAttributeDescription attrDescs[5]{};
        attrDescs[0].binding    = 0;
        attrDescs[0].location   = 0;
        attrDescs[0].format     = VK_FORMAT_R32G32B32_SFLOAT;
        attrDescs[0].offset     = offsetof(Vertex, pos);

        // Attribute 1: color (vec3, offset after pos)
        attrDescs[1].binding    = 0;
        attrDescs[1].location   = 1;
        attrDescs[1].format     = VK_FORMAT_R32G32B32_SFLOAT;
        attrDescs[1].offset     = offsetof(Vertex, color);

        // UV
        attrDescs[2].binding    = 0;
        attrDescs[2].location   = 2;
        attrDescs[2].format     = VK_FORMAT_R32G32_SFLOAT;
        attrDescs[2].offset     = offsetof(Vertex, uv);

        // normals
        attrDescs[3].binding    = 0;
        attrDescs[3].location   = 3;
        attrDescs[3].format     = VK_FORMAT_R32G32B32_SFLOAT;
        attrDescs[3].offset     = offsetof(Vertex, normal);

        // tangents
        attrDescs[4].binding    = 0;
        attrDescs[4].location   = 4;
        attrDescs[4].format     = VK_FORMAT_R32G32B32_SFLOAT;
        attrDescs[4].offset     = offsetof(Vertex, tangent);

        VkPipelineVertexInputStateCreateInfo vertexInput{};
        vertexInput.sType                               = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vertexInput.vertexBindingDescriptionCount       = 1;
        vertexInput.pVertexBindingDescriptions          = &bindingDesc;
        vertexInput.vertexAttributeDescriptionCount     = 5;
        vertexInput.pVertexAttributeDescriptions        = attrDescs;

        // 3. Input assembly: which primitive the vertices form
        VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
        inputAssembly.sType     = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        inputAssembly.topology  = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

        // 4-8. All the state it shares with the skinned mesh pipeline
        // and with the four outline ones: dynamic viewport and scissor, solid
        // fill with back-face culling, LESS depth with write, and opaque
        // without blending. It lives in PipelineDefaults.h since H10; it was written
        // here and again in createSkinnedGraphicsPipelines, with the same
        // values. Careful: it CANNOT be copied, its pointers point at its own
        // members and it has to stay alive until vkCreateGraphicsPipelines.
        const GraphicsPipelineState estadoComun(m_aaSampleCount);

        // 9. Pipeline layout: no descriptors or push constants for now
        VkPushConstantRange pushRange{};
        pushRange.stageFlags    = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        pushRange.offset        = 0;
        pushRange.size          = sizeof(PushData);   // 80 bytes

        // Set 0: UBO + textures (per shared entry). Set 1: SSBO of
        // per-instance transforms, one per frame, bound once per
        // pass. This layout is shared by the static pipeline, the wireframe and the
        // skinned one; the skinned one does not read set 1 (useInstancing = 0), but since
        // it shares the layout the set stays bound and is valid.
        // Set 2: the Forward+ buffers (parameters, lights, grid and indices).
        // One per frame, bound once per pass. Its own set and not new bindings
        // of set 0 because that one only had 8 free and extending it would force
        // rewriting the descriptor set of EACH object. Only pbr.frag reads it; the
        // wireframe, the skinned and the outline share the layout and do not declare it,
        // which is legal as long as they do not use it.
        VkDescriptorSetLayout setLayouts[] = { m_descriptorSetLayout, m_instanceBuffers.descLayout(), m_fpPass.descLayout() };

        VkPipelineLayoutCreateInfo layoutInfo{};
        layoutInfo.sType                    = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layoutInfo.setLayoutCount           = 3;
        layoutInfo.pSetLayouts              = setLayouts;
        layoutInfo.pushConstantRangeCount   = 1;
        layoutInfo.pPushConstantRanges      = &pushRange;
        // Idempotence guard: recreateMsaaDependentPipelines comes back in
        // here to redo the four pipelines with another sample count, and
        // the layout does not depend on that.
        if(m_pipelineLayout == VK_NULL_HANDLE &&
           vkCreatePipelineLayout(m_gpu.device(), &layoutInfo, nullptr, &m_pipelineLayout) != VK_SUCCESS)
        {
            throw std::runtime_error("failed to create pipeline layout!");
        }

        // 10. Complete pipeline
        VkGraphicsPipelineCreateInfo pipelineInfo{};
        estadoComun.fill(pipelineInfo);
        // And what belongs to THIS pipeline and not to the common one: its shaders, its vertex
        // input, the layout and which render pass it is compiled against.
        pipelineInfo.stageCount             = 2;
        pipelineInfo.pStages                = stages;
        pipelineInfo.pVertexInputState      = &vertexInput;
        pipelineInfo.layout                 = m_pipelineLayout;
        pipelineInfo.renderPass             = m_offscreenRenderPass;

        if(vkCreateGraphicsPipelines(m_gpu.device(), VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_pipeline) != VK_SUCCESS)
        {
            throw std::runtime_error("failed to create graphics pipeline!");
        }

        // Wireframe pipeline: same vertex input/layout/render pass, only
        // polygonMode changes to LINE and the fragment shader to flat color.
        auto wireFragCode = loadShaderFile("shaders/wireframe.frag.spv");
        VkShaderModule wireFragModule = createShaderModule(wireFragCode);

        VkPipelineShaderStageCreateInfo wireFragStage{};
        wireFragStage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        wireFragStage.stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
        wireFragStage.module = wireFragModule;
        wireFragStage.pName  = "main";

        VkPipelineShaderStageCreateInfo wireStages[] = { vertStage, wireFragStage };

        VkPipelineRasterizationStateCreateInfo wireRasterizationInfo = estadoComun.rasterization;
        wireRasterizationInfo.polygonMode = VK_POLYGON_MODE_LINE;

        VkGraphicsPipelineCreateInfo wirePipelineInfo = pipelineInfo;
        wirePipelineInfo.pStages             = wireStages;
        wirePipelineInfo.pRasterizationState = &wireRasterizationInfo;

        if(vkCreateGraphicsPipelines(m_gpu.device(), VK_NULL_HANDLE, 1, &wirePipelineInfo, nullptr, &m_wireframePipeline) != VK_SUCCESS)
        {
            throw std::runtime_error("failed to create wireframe graphics pipeline!");
        }

        // Selection outline pipeline: same vertex input, same layout and
        // same render pass as the one above; only the two shaders and the
        // culling change, which now discards FRONT faces. The back faces of the
        // extruded hull stay behind the object's surface, so
        // the depth test (LESS) only lets through the rim that sticks out of its
        // silhouette. depthWrite stays TRUE on purpose: that rim has to
        // write depth or the skybox, which draws with LEQUAL where nothing
        // wrote, would cover it.
        m_outlinePass.createStaticPipelines(outlineCtx(), pipelineInfo, estadoComun.rasterization,
                                           vertexInput, offsetof(Vertex, pos),
                                           offsetof(Vertex, normal));

        // the modules are destroyed at the end of this function; only the pipeline needs them
        vkDestroyShaderModule(m_gpu.device(), vertModule, nullptr);
        vkDestroyShaderModule(m_gpu.device(), fragModule, nullptr);
        vkDestroyShaderModule(m_gpu.device(), wireFragModule, nullptr);
        // The outline ones are loaded and released by SelectionOutlinePass.
        printf("pipeline OK\n"); fflush(stdout);
    }

    std::vector<char> Renderer::loadShaderFile(const std::string& path)
    {
        std::ifstream file(path, std::ios::ate | std::ios::binary);
        if(!file.is_open())
        {
            throw std::runtime_error("failed to open shader: " + path);
        }
        size_t size = (size_t)file.tellg();
        std::vector<char> buffer(size);
        file.seekg(0);
        file.read(buffer.data(), (std::streamsize)size);
        return buffer;
    }

    VkShaderModule Renderer::createShaderModule(const std::vector<char>& code)
    {
        VkShaderModuleCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        createInfo.codeSize = code.size();
        createInfo.pCode = reinterpret_cast<const uint32_t *>(code.data());
        VkShaderModule shaderModule;
        if(vkCreateShaderModule(m_gpu.device(), &createInfo, nullptr, &shaderModule) != VK_SUCCESS)
        {
            throw std::runtime_error("failed to create shader module!");
        }
        return shaderModule;
    }

    void Renderer::setPresentMode(PresentMode v)
    {
        if (v == presentMode()) return;
        setPresentModeFlag(v);
        // The mode lives INSIDE the swapchain, so changing it forces a rebuild.
        // The same signal as a window resize is reused instead of recreating it
        // here: that path already waits for the GPU to be idle and redoes everything
        // that hangs off the swapchain, and doing it by hand would be a second copy
        // of that teardown.
        m_framebufferResized = true;
    }

    bool Renderer::presentModeSupported(PresentMode v) const
    {
        switch (v)
        {
            // FIFO is guaranteed by the Vulkan spec: there is no need to query it, and
            // returning false here would leave the UI with no valid option.
            case PresentMode::Vsync:     return true;
            case PresentMode::Mailbox:   return m_mailboxDisponible;
            case PresentMode::Immediate: return m_immediateDisponible;
        }
        return false;
    }

    void Renderer::recreateSwapChain(Window& window)
    {
        int width = 0;
        int height = 0;

        glfwGetFramebufferSize(window.getNativeWindow(), &width, &height);
        // Wait if the window is minimized (0x0)
        while(width == 0 || height == 0)
        {
            glfwWaitEvents();
            glfwGetFramebufferSize(window.getNativeWindow(), &width, &height);
        }

        vkDeviceWaitIdle(m_gpu.device());

        // Offscreen teardown first (its FBs use m_depthImageView)
        destroyOffscreenImages();

        // Teardown swapchain
        for (auto semaphore : m_renderFinished)
            vkDestroySemaphore(m_gpu.device(), semaphore, nullptr);
        m_renderFinished.clear();

        for (auto fb : m_swapChainFramebuffers)
            vkDestroyFramebuffer(m_gpu.device(), fb, nullptr);
        m_swapChainFramebuffers.clear();

        for (auto iv : m_swapChainImageViews)
            vkDestroyImageView(m_gpu.device(), iv, nullptr);
        m_swapChainImageViews.clear();

        vkDestroyImageView(m_gpu.device(), m_depthImageView, nullptr);
        vkDestroyImage(m_gpu.device(), m_depthImage, nullptr);
        vkFreeMemory(m_gpu.device(), m_depthImageMemory, nullptr);
        vkDestroySwapchainKHR(m_gpu.device(), m_swapChain, nullptr);
        m_swapChain = VK_NULL_HANDLE;

        // Recreate
        createSwapChain(window);
        createImageViews();
        createDepthResources();
        createFramebuffers();
        createOffscreenImages(); // recreated with the new size

        // Only recreate the semaphores that depend on the image count
        m_renderFinished.resize(m_swapChainImages.size());
        VkSemaphoreCreateInfo semInfo{};
        semInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        for (auto& sem : m_renderFinished)
            if (vkCreateSemaphore(m_gpu.device(), &semInfo, nullptr, &sem) != VK_SUCCESS)
                throw std::runtime_error("failed to create renderFinished semaphore!");
    }

    void Renderer::createVertexBuffer(const std::vector<Vertex>& vertices, VkBuffer& buf, VkDeviceMemory& mem, TransferBatch* batch)
    {
        VkDeviceSize size = sizeof(vertices[0]) * vertices.size();

        VkBuffer stagingBuffer;
        VkDeviceMemory stagingMemory;
        m_res.createBuffer(size,
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            stagingBuffer, stagingMemory);

        void* data;
        vkMapMemory(m_gpu.device(), stagingMemory, 0, size, 0, &data);
        memcpy(data, vertices.data(), (size_t)size);
        vkUnmapMemory(m_gpu.device(), stagingMemory);

        m_res.createBuffer(size,
            VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
            buf, mem);

        m_res.copyBuffer(stagingBuffer, buf, size, batch);

        // With a batch, the copy runs when the fence is submitted: destroying the staging
        // now would be a use-after-free on the GPU. The batch owns it until it
        // signals. Without a batch, the copy already waited (vkQueueWaitIdle) and it is released now.
        if (batch)
            batch->addStaging(stagingBuffer, stagingMemory);
        else
        {
            vkDestroyBuffer(m_gpu.device(), stagingBuffer, nullptr);
            vkFreeMemory(m_gpu.device(), stagingMemory, nullptr);
        }
    }

    void Renderer::createIndexBuffer(const std::vector<uint32_t>& idx, VkBuffer& buf, VkDeviceMemory& mem, TransferBatch* batch)
    {
        VkDeviceSize size = sizeof(idx[0]) * idx.size();

        VkBuffer stagingBuffer;
        VkDeviceMemory stagingMemory;
        m_res.createBuffer(size,
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            stagingBuffer, stagingMemory);

        void* data;
        vkMapMemory(m_gpu.device(), stagingMemory, 0, size, 0, &data);
        memcpy(data, idx.data(), (size_t)size);
        vkUnmapMemory(m_gpu.device(), stagingMemory);

        m_res.createBuffer(size,
            VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
            buf, mem);

        m_res.copyBuffer(stagingBuffer, buf, size, batch);

        // See createVertexBuffer: the staging lives until the fence when there is a batch.
        if (batch)
            batch->addStaging(stagingBuffer, stagingMemory);
        else
        {
            vkDestroyBuffer(m_gpu.device(), stagingBuffer, nullptr);
            vkFreeMemory(m_gpu.device(), stagingMemory, nullptr);
        }
    }

    void Renderer::createDescriptorSetLayout()
    {
        VkDescriptorSetLayoutBinding uboBinding{};
        uboBinding.binding          = 0;
        uboBinding.descriptorType   = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        uboBinding.descriptorCount  = 1;
        uboBinding.stageFlags       = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

        VkDescriptorSetLayoutBinding samplerBinding{};
        samplerBinding.binding         = 1;
        samplerBinding.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        samplerBinding.descriptorCount = 1;
        samplerBinding.stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;

        VkDescriptorSetLayoutBinding samplerNormal{};
        samplerNormal.binding         = 2;
        samplerNormal.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        samplerNormal.descriptorCount = 1;
        samplerNormal.stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;

        VkDescriptorSetLayoutBinding shadowBinding{};
        shadowBinding.binding         = 3;
        shadowBinding.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        shadowBinding.descriptorCount = 1;
        shadowBinding.stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;

        VkDescriptorSetLayoutBinding ormBinding{};
        ormBinding.binding         = 4;
        ormBinding.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        ormBinding.descriptorCount = 1;
        ormBinding.stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;

        // IBL: irradiance (diffuse) and prefiltered environment (specular). Only
        // pbr.frag consumes them, but the layout is shared by the three pipelines
        // via m_pipelineLayout, so they go here all the same.
        VkDescriptorSetLayoutBinding irradianceBinding{};
        irradianceBinding.binding         = 5;
        irradianceBinding.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        irradianceBinding.descriptorCount = 1;
        irradianceBinding.stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;

        VkDescriptorSetLayoutBinding prefilterBinding{};
        prefilterBinding.binding         = 6;
        prefilterBinding.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        prefilterBinding.descriptorCount = 1;
        prefilterBinding.stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;

        // SSAO. Same criterion as the two above: only pbr.frag reads it, but
        // the layout is shared by the static, wireframe, skinned and
        // outline pipelines via m_pipelineLayout.
        VkDescriptorSetLayoutBinding ssaoBinding{};
        ssaoBinding.binding         = 7;
        ssaoBinding.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        ssaoBinding.descriptorCount = 1;
        ssaoBinding.stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;

        VkDescriptorSetLayoutBinding bindings[] = { uboBinding, samplerBinding, samplerNormal, shadowBinding, ormBinding,
                                                    irradianceBinding, prefilterBinding, ssaoBinding };

        VkDescriptorSetLayoutCreateInfo layoutInfo{};
        layoutInfo.sType            = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layoutInfo.bindingCount     = 8;
        layoutInfo.pBindings        = bindings;

        if(vkCreateDescriptorSetLayout(m_gpu.device(), &layoutInfo, nullptr, &m_descriptorSetLayout) != VK_SUCCESS)
        {
            throw std::runtime_error("failed to create descriptor set layout!");
        }
    }

    void Renderer::createUniformBuffers()
    {
        VkDeviceSize size = sizeof(UniformBufferObject);

        // That the block FITS in a UBO of this GPU. It is dominated by MAX_LIGHTS (64
        // lights are 4 KB of the ~5 it takes up), so raising it further is what
        // can go over the line. The spec guarantees 16 KB as a minimum, but
        // the spec minimum is not the real limit (H72): it is read from the device.
        //
        // Going over does not give a useful error (vkCreateBuffer swallows it, and what fails is
        // the descriptor binding later), so a warning is given here with the
        // two numbers and with the cause.
        const uint32_t topeUbo = m_gpu.maxUniformBufferRange();
        if (topeUbo > 0 && size > topeUbo)
        {
            printf("WARNING: the UBO block takes %llu bytes and this GPU allows at most %u.\n"
                   "       MAX_LIGHTS (%d lights) dominates it: lowering it is what brings it back\n"
                   "       within the limit.\n",
                   (unsigned long long)size, topeUbo, MAX_LIGHTS);
            fflush(stdout);
        }

        for (int i = 0; i < MAX_FRAMES; i++)
        {
            VkBufferCreateInfo bufferInfo{};
            bufferInfo.sType        = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            bufferInfo.size         = size;
            bufferInfo.usage        = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
            bufferInfo.sharingMode  = VK_SHARING_MODE_EXCLUSIVE;
            // All four VkResults are checked: the last one leaves a pointer in
            // m_uniformBuffersMapped that updateUniformBuffer uses with memcpy on
            // EVERY frame. Ignoring them turned a memory failure into a
            // write through an uninitialized pointer, which is a crash with no
            // apparent relation to its cause.
            if (vkCreateBuffer(m_gpu.device(), &bufferInfo, nullptr, &m_uniformBuffers[i]) != VK_SUCCESS)
                throw std::runtime_error("failed to create uniform buffer!");

            VkMemoryRequirements memoryRequirements;
            vkGetBufferMemoryRequirements(m_gpu.device(), m_uniformBuffers[i], &memoryRequirements);

            VkMemoryAllocateInfo allocInfo{};
            allocInfo.sType             = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            allocInfo.allocationSize    = memoryRequirements.size;
            allocInfo.memoryTypeIndex   = m_gpu.findMemoryType(memoryRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            if (vkAllocateMemory(m_gpu.device(), &allocInfo, NULL, &m_uniformBuffersMemory[i]) != VK_SUCCESS)
                throw std::runtime_error("failed to allocate uniform buffer memory!");
            if (vkBindBufferMemory(m_gpu.device(), m_uniformBuffers[i], m_uniformBuffersMemory[i], 0) != VK_SUCCESS)
                throw std::runtime_error("failed to bind uniform buffer memory!");

            // Persistent mapping: we never call unmap
            if (vkMapMemory(m_gpu.device(), m_uniformBuffersMemory[i], 0, size, 0, &m_uniformBuffersMapped[i]) != VK_SUCCESS)
                throw std::runtime_error("failed to map uniform buffer!");

        }
    }

    uint32_t Renderer::buildInstanceBatches(const BatchCandidate* candidates,
                                            size_t                count,
                                            glm::mat4*            outTransforms,
                                            uint32_t              outCapacity,
                                            uint32_t              firstInstanceBase,
                                            std::vector<InstanceBatch>& outBatches)
    {
        // The grouping lives in Renderer/InstanceBatching.{h,cpp} since there is a
        // second backend that needs it: it has no line of Vulkan, and
        // dragging vulkan.h into the DirectX 12 one for this makes no sense. This
        // delegate stays so as not to touch the callers or instancing_tests.
        return Batching::buildInstanceBatches(candidates, count, outTransforms, outCapacity,
                                              firstInstanceBase, outBatches);
    }

    void Renderer::gatherAndBatch(const Frustum& frustum, bool colorPass)
    {
        // The guards and the culling are PER OBJECT and are resolved here, which is
        // where the GPU cache is; the grouping only sees the result in
        // `visible`. Grouping before culling would draw too much.
        //
        // The three passes went through here with the loop written by hand, and the
        // three copies had to match: if they diverge, the AO darkens against
        // geometry that is not drawn and the shadows float without an object. Now the
        // decision is in Visibility::gatherCandidates, with its test.
        Visibility::gatherCandidates(m_objects, m_sharedMeshes, m_lastCompletedTicket,
                                     frustum, colorPass && m_ssrEnabled, colorPass,
                                     m_batchCandidates);

        // Own stretch inside the frame's SSBO: the passes share the buffer and
        // the cursor marks where this one's starts, which is the base of its
        // firstInstance.
        const InstanceCursor::Span libre = m_instanceBuffers.cur().rest();
        m_instanceBuffers.cur().commit(
            buildInstanceBatches(m_batchCandidates.data(), m_batchCandidates.size(),
                                 libre.data, libre.capacity, libre.base, m_instanceBatches));
    }

    void Renderer::createDescriptorPool()
    {
        // The first of the chain. Before this was the ONLY one, sized with the
        // meshes there were at startup plus 128 of margin; past the margin,
        // vkAllocateDescriptorSets failed and it threw in the middle of a Load
        // Scene. Now the size is fixed and allocateSharedSets chains another one
        // when needed.
        if (!addDescriptorPool())
            throw std::runtime_error("failed to create descriptor pool!");
    }

    bool Renderer::addDescriptorPool()
    {
        const uint32_t n = kSharedSetsPerPool;
        VkDescriptorPoolSize poolSizes[2]{};
        poolSizes[0].type            = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        poolSizes[0].descriptorCount = n;
        poolSizes[1].type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        poolSizes[1].descriptorCount = n * 7;   // diffuse + normal map + shadow + orm + irradiance + prefiltered + ssao

        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.poolSizeCount = 2;
        poolInfo.pPoolSizes    = poolSizes;
        poolInfo.maxSets       = n;
        // FREE_DESCRIPTOR_SET: rebuildSkinnedMesh destroys and recreates the object in
        // place, and without being able to return the sets to the pool each reimport
        // would consume slots until it ran out.
        poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;

        VkDescriptorPool pool = VK_NULL_HANDLE;
        if (vkCreateDescriptorPool(m_gpu.device(), &poolInfo, nullptr, &pool) != VK_SUCCESS)
            return false;

        m_descriptorPools.push_back(pool);
        return true;
    }

    VkDescriptorPool Renderer::allocateSharedSets(VkDescriptorSet* outSets)
    {
        VkDescriptorSetLayout layouts[MAX_FRAMES] = { m_descriptorSetLayout, m_descriptorSetLayout };

        VkDescriptorSetAllocateInfo allocInfo{};
        allocInfo.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocInfo.descriptorSetCount = MAX_FRAMES;
        allocInfo.pSetLayouts        = layouts;

        // Two attempts: the last pool and, if it is full, a newly created one. The
        // earlier ones are not walked: on freeing, the holes go back to THEIR pool and
        // could be reused, but looking for them would cost a failed call
        // per pool on the normal path. What is lost is memory, not
        // correctness.
        for (int intento = 0; intento < 2; ++intento)
        {
            if (!m_descriptorPools.empty())
            {
                allocInfo.descriptorPool = m_descriptorPools.back();
                const VkResult r = vkAllocateDescriptorSets(m_gpu.device(), &allocInfo, outSets);
                if (r == VK_SUCCESS)
                    return m_descriptorPools.back();
                if (r != VK_ERROR_OUT_OF_POOL_MEMORY && r != VK_ERROR_FRAGMENTED_POOL)
                    return VK_NULL_HANDLE;
            }
            if (!addDescriptorPool())
                return VK_NULL_HANDLE;
        }
        return VK_NULL_HANDLE;
    }

    void Renderer::createDescriptorSets()
    {
        // Per shared entry, not per object: initSceneResources builds
        // all the RenderObjects first (no pool yet) and then calls here.
        // Iterating m_objects would allocate N sets for the same entry and the first N-1
        // would be lost.
        for (int index : m_sharedMeshes.liveIndices())
            allocateObjectDescriptorSet(*m_sharedMeshes.get(index));
    }

    void Renderer::allocateObjectDescriptorSet(SharedGpuMesh& obj)
    {
        obj.descPool = allocateSharedSets(obj.descriptorSets);
        if (obj.descPool == VK_NULL_HANDLE)
            throw std::runtime_error("failed to allocate descriptor sets!");

        for (int i = 0; i < MAX_FRAMES; i++)
        {
            VkDescriptorBufferInfo bufferInfo{};
            bufferInfo.buffer = m_uniformBuffers[i];
            bufferInfo.offset = 0;
            bufferInfo.range  = sizeof(UniformBufferObject);

            VkDescriptorImageInfo imageInfo{};
            imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            imageInfo.imageView   = obj.textureView;
            imageInfo.sampler     = obj.sampler;

            VkDescriptorImageInfo normalInfo{};
            normalInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            normalInfo.imageView   = obj.normalView;
            normalInfo.sampler     = obj.normalSampler;

            VkDescriptorImageInfo shadowInfo{};
            shadowInfo.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
            shadowInfo.imageView   = m_shadowPass.view();
            shadowInfo.sampler     = m_shadowPass.sampler();

            VkDescriptorImageInfo ormInfo{};
            ormInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            ormInfo.imageView   = obj.ormView;
            ormInfo.sampler     = obj.ormSampler;

            // IBL: the same two cubemaps for all the objects. They exist since
            // init(), so these writes are valid even without a skybox. Both
            // descriptors are filled in by IblPass, further down.

            VkWriteDescriptorSet writes[7]{};
            writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; writes[0].dstSet = obj.descriptorSets[i];
            writes[0].dstBinding = 0; writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            writes[0].descriptorCount = 1; writes[0].pBufferInfo = &bufferInfo;

            writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; writes[1].dstSet = obj.descriptorSets[i];
            writes[1].dstBinding = 1; writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[1].descriptorCount = 1; writes[1].pImageInfo = &imageInfo;

            writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; writes[2].dstSet = obj.descriptorSets[i];
            writes[2].dstBinding = 2; writes[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[2].descriptorCount = 1; writes[2].pImageInfo = &normalInfo;

            writes[3].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; writes[3].dstSet = obj.descriptorSets[i];
            writes[3].dstBinding = 3; writes[3].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[3].descriptorCount = 1; writes[3].pImageInfo = &shadowInfo;

            writes[4].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; writes[4].dstSet = obj.descriptorSets[i];
            writes[4].dstBinding = 4; writes[4].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[4].descriptorCount = 1; writes[4].pImageInfo = &ormInfo;

            // The two IBL ones, from the single place: the characters and the probe pass
            // also write them.
            VkDescriptorImageInfo iblInfos[2]{};
            IblPass::fillIblWrites(obj.descriptorSets[i], m_iblPass.irradianceView(),
                                   m_iblPass.prefilterView(), m_iblPass.sampler(),
                                   iblInfos, &writes[5]);

            vkUpdateDescriptorSets(m_gpu.device(), 7, writes, 0, nullptr);

            // Binding 7 (SSAO) apart: it is the only view of this set that is
            // destroyed and redone on resize, and refreshSsaoDescriptors
            // comes back through here with the same handles.
            writeSsaoBinding(obj.descriptorSets[i], i);
        }
    }

    void Renderer::writeSsaoBinding(VkDescriptorSet set, int frameIndex)
    {
        // With no image yet (early init) there is nothing valid to write: the
        // set is completed from refreshSsaoDescriptors as soon as it exists.
        if (m_ssaoPass.blurViews()[frameIndex] == VK_NULL_HANDLE) return;

        VkDescriptorImageInfo info{};
        // GENERAL and not SHADER_READ_ONLY: the same image is the compute's storage image
        // and the scene pass's texture, just like the bloom chain.
        info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        info.imageView   = m_ssaoPass.blurViews()[frameIndex];
        info.sampler     = m_depthPrepass.sampler();

        VkWriteDescriptorSet w{};
        w.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w.dstSet          = set;
        w.dstBinding      = 7;
        w.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w.descriptorCount = 1;
        w.pImageInfo      = &info;
        vkUpdateDescriptorSets(m_gpu.device(), 1, &w, 0, nullptr);
    }

    void Renderer::refreshSsaoDescriptors()
    {
        for (int index : m_sharedMeshes.liveIndices())
        {
            SharedGpuMesh* gpu = m_sharedMeshes.get(index);
            for (int i = 0; i < MAX_FRAMES; i++)
                if (gpu->descriptorSets[i]) writeSsaoBinding(gpu->descriptorSets[i], i);
        }
        // The skinned ones have their own sets of the same layout, one per material.
        for (const SkinnedRenderObject& sobj : m_skinnedObjects)
            for (const SkinnedMatGfx& mgfx : sobj.matGfx)
                for (int i = 0; i < MAX_FRAMES; i++)
                    if (mgfx.descSets[i]) writeSsaoBinding(mgfx.descSets[i], i);
    }

    void Renderer::setSsaoEnabled(bool v)
    {
        if (v == ssaoEnabled()) return;
        setSsaoEnabledFlag(v);
        // When turned off, the map keeps the AO of the last computed frame and
        // would keep darkening. A clear to 1.0 per in-flight frame returns it to
        // identity; from then on, zero work.
        if (!v)
            m_ssaoPass.markClearPending();
    }

    void Renderer::updateUniformBuffer(uint32_t frameIndex)
    {
        // The CameraComponent's camera in Play, the editor's in edit mode. The
        // Vulkan Y-flip is already applied from currentFrameCamera().
        const FrameCamera fc = currentFrameCamera();

        // View-proj WITHOUT jitter: it is the one the TAA reprojects with and the one compared
        // with the previous frame's. The jitter is sampling noise, not
        // camera motion, and putting it in here would drag the history.
        //
        // The prev←curr handoff is done here and on ALL frames because the motion
        // blur also reprojects with these two, and it runs with the TAA off. The
        // equivalent line at the end of the TAA pass stays where it is and
        // writes exactly the same value: with the TAA active this is
        // redundant, not a change.
        m_aaPass.updateFrameMatrices(aaCtx(), fc.view, fc.proj);

        UniformBufferObject ubo{};
        ubo.view = fc.view;
        // With TAA it comes out jittered; in any other mode it is fc.proj as is.
        ubo.proj = m_aaPass.jitteredProj();
        ubo.numLights        = std::min((int)m_lights.size(), MAX_LIGHTS);
        ubo.ambientIntensity = m_ambientEnabled ? m_ambientIntensity : 0.0f;
        for(int i = 0; i < ubo.numLights; i++)
        {
            ubo.lights[i] = m_lights[i];
        }
        
        ubo.viewPos  = glm::vec4(fc.eye, 1.0f);
        // The cascades were already computed by draw() for this frame. Copying them and not
        // recomputing them is what guarantees that the fragment shader samples with
        // exactly the same matrices with which the shadow pass was culled and
        // recorded.
        // All SIX, not four: with a point light faces 4 and 5 also
        // carry a matrix. Copying only 4 would leave two faces with the identity.
        for (int i = 0; i < SHADOW_MATRICES; i++)
        {
            ubo.lightSpaceMatrix[i] = m_shadowPass.cascadeMatrix(i);
        }
        ubo.cascadeSplits = m_shadowPass.cascadeSplits();
        // position.w tells the shader WHICH shadow each light has. That slot was
        // free: no shader read position.w.
        //
        //   light 0 (key):  1 = its shadow was recorded as a CUBEMAP, 0 = cascades or
        //                   a single face, which the shader tells apart by type.
        //   lights 1..n:    matrix index + 1, and 0 = casts no shadow.
        //
        // Everything comes from what the pass DID (the layers it left valid and the
        // slots it handed out), not from recomputing any criterion here: two
        // copies of a criterion is what broke H65.
        if (ubo.numLights > 0)
        {
            ubo.lights[0].position.w =
                (m_shadowPass.activeLayers() == SHADOW_KEY_MATRICES) ? 1.0f : 0.0f;

            // Slot + 1, with the SIGN telling which path it was recorded through:
            // positive = one face, negative = six-face cubemap. It goes in the same
            // field because there is no other free one in the block, and it comes from what the
            // pass did (shadowFaceCounts) instead of the shader deducing it
            // from the light type: deducing it would be a second copy of the criterion,
            // which is what broke H65.
            const int* ranuras = m_shadowPass.shadowSlots();
            const int* caras   = m_shadowPass.shadowFaceCounts();
            for (int i = 1; i < ubo.numLights; i++)
            {
                if (ranuras[i] < 0) { ubo.lights[i].position.w = 0.0f; continue; }
                const float codigo = (float)(ranuras[i] + 1);
                ubo.lights[i].position.w = (caras[i] == SHADOW_KEY_MATRICES) ? -codigo : codigo;
            }
        }

        memcpy(m_uniformBuffersMapped[frameIndex], &ubo, sizeof(ubo));
        // A reflection probe's bake starts from this same buffer (lights and the
        // frame's cascade matrices); until it is written once it is garbage.
        m_uboWritten[frameIndex] = true;

        // ── Forward+: parameter block and light list ─────────────────────────
        // The frame it applies to is not m_currentFrame but frameIndex (the probe
        // bake writes 0), so the context is built with that one.
        ForwardPlusPass::Context fpc = fpCtx();
        fpc.currentFrame = frameIndex;
        m_fpPass.uploadFrameData(fpc, fc.view, fc.proj, m_lights, m_lightRadii, m_fpLightRadius);
    }

    void Renderer::createDepthResources()
    {
        VkFormat depthFormat = VK_FORMAT_D32_SFLOAT;

        VkImageCreateInfo imageInfo{};
        imageInfo.sType             = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType         = VK_IMAGE_TYPE_2D;
        imageInfo.format            = depthFormat;
        // INTERNAL size and samples: this depth is shared by the scene pass and
        // the composition one, so it follows SSAA (more resolution) and MSAA
        // (more samples). The SSAO/SSR pre-pass one is another and always runs at one.
        imageInfo.extent            = { m_renderExtent.width, m_renderExtent.height, 1 };
        imageInfo.mipLevels         = 1;
        imageInfo.arrayLayers       = 1;
        imageInfo.samples           = m_aaSampleCount;
        imageInfo.tiling            = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage             = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        imageInfo.sharingMode       = VK_SHARING_MODE_EXCLUSIVE;
        imageInfo.initialLayout     = VK_IMAGE_LAYOUT_UNDEFINED;

        if(vkCreateImage(m_gpu.device(), &imageInfo, nullptr, &m_depthImage) != VK_SUCCESS)
        {
            throw std::runtime_error("failed to create depth image!");
        }

        VkMemoryRequirements memReq;
        vkGetImageMemoryRequirements(m_gpu.device(), m_depthImage, &memReq);

        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType             = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize    = memReq.size;
        allocInfo.memoryTypeIndex   = m_gpu.findMemoryType(memReq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if(vkAllocateMemory(m_gpu.device(), &allocInfo, nullptr, &m_depthImageMemory) != VK_SUCCESS)
        {
            throw std::runtime_error("failed to allocate depth image memory!");
        }

        vkBindImageMemory(m_gpu.device(), m_depthImage, m_depthImageMemory, 0);

        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = m_depthImage;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = depthFormat;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.baseMipLevel = 0;
        viewInfo.subresourceRange.layerCount = 1;

        if(vkCreateImageView(m_gpu.device(), &viewInfo, nullptr, &m_depthImageView) != VK_SUCCESS)
        {
            throw std::runtime_error("failed to create depth image view!");
        }
    }

    bool Renderer::buildRenderObject(const Mesh& mesh, RenderObject& obj,
                                     TransferBatch* batch,
                                     const std::vector<DecodedImage>* decoded)
    {
        obj.name = mesh.name;

        bool created = false;
        obj.sharedIndex = m_sharedMeshes.acquire(
            makeSharedMeshKey(mesh),
            [&](SharedGpuMesh& gpu) { createSharedGpuMesh(mesh, gpu, batch, decoded); },
            &created);

        // The OBJECT's factors, here and not in the shared entry: it is what
        // lets two objects with the same mesh and different finish
        // share VRAM. They are read from the entry (not from the material) if it has an
        // ORM map, which is the only one that can answer that for a
        // reused entry (created == false, when createSharedGpuMesh does not even run).
        if (const SharedGpuMesh* gpu = m_sharedMeshes.get(obj.sharedIndex))
            setEffectiveFactors(obj, *gpu, mesh.material.metallic, mesh.material.roughness);
        return created;
    }

    void Renderer::setObjectMaterialFactors(size_t objectIndex, float metallic, float roughness)
    {
        if (objectIndex >= m_objects.size()) return;
        RenderObject& obj = m_objects[objectIndex];
        // Without a shared entry there is nobody to ask about the ORM map, and
        // there is nothing to draw either: the object is unbuilt or already
        // released. It returns without writing, which is the same thing the other
        // setters do with an index that describes nothing.
        const SharedGpuMesh* gpu = m_sharedMeshes.get(obj.sharedIndex);
        if (!gpu) return;
        setEffectiveFactors(obj, *gpu, metallic, roughness);
    }

    void Renderer::setEffectiveFactors(RenderObject& obj, const SharedGpuMesh& gpu,
                                       float metallic, float roughness)
    {
        // The "with an ORM map the map rules" rule, in ONE place: it is shared by the
        // object's registration and setObjectMaterialFactors, which is where the
        // sliders come in. Written twice, dragging a slider over an
        // object with an ORM map would have put the slider's value where the
        // registration puts 1.0, and the object would have changed look depending on which
        // path touched it last.
        obj.metallic  = gpu.hasOrmMap ? 1.0f : metallic;
        obj.roughness = gpu.hasOrmMap ? 1.0f : roughness;
    }

    void Renderer::createSharedGpuMesh(const Mesh& mesh, SharedGpuMesh& obj,
                                       TransferBatch* batch,
                                       const std::vector<DecodedImage>* decoded)
    {
        obj.indexCount = (uint32_t)mesh.indices.size();

        // Local AABB for frustum culling. It is computed here and not in the draw
        // loop because the geometry does not change afterwards: the only thing that
        // moves is the RenderObject's transform, and the test already transforms it
        // every frame.
        obj.hasBounds = !mesh.vertices.empty();
        if (obj.hasBounds)
        {
            obj.aabbMin = glm::vec3(std::numeric_limits<float>::max());
            obj.aabbMax = glm::vec3(std::numeric_limits<float>::lowest());
            for (const Vertex& v : mesh.vertices)
            {
                obj.aabbMin = glm::min(obj.aabbMin, v.pos);
                obj.aabbMax = glm::max(obj.aabbMax, v.pos);
            }
        }

        createVertexBuffer(mesh.vertices, obj.vertexBuffer, obj.vertexMemory, batch);
        createIndexBuffer(mesh.indices,   obj.indexBuffer,  obj.indexMemory,  batch);

        // Looks up a slot among the pixels the worker already decoded. With no
        // hit it falls back to the usual path (stbi_load on this thread), which is
        // what happens in the synchronous init and with undecoded textures.
        auto findSlot = [decoded](DecodedImage::Slot s) -> const DecodedImage* {
            if (!decoded) return nullptr;
            for (const auto& d : *decoded) if (d.slot == s) return &d;
            return nullptr;
        };

        // The format of each image is decided by resolveSrgb (slot + sidecar
        // settings) and the view has to declare EXACTLY the same one: the image
        // is not created with MUTABLE_FORMAT.
        VkFormat albedoFmt = VK_FORMAT_R8G8B8A8_SRGB;
        if (const DecodedImage* albedo = findSlot(DecodedImage::Albedo))
        {
            albedoFmt = resolveSrgb(TextureKind::BaseColor, albedo->colorSpace)
                            ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
            m_res.createMaterialImageFromPixels(albedo->pixels.data(),
                                                (uint32_t)albedo->w, (uint32_t)albedo->h, albedoFmt,
                                                albedo->mips.data(), albedo->mips.size(),
                                                obj.textureImage, obj.textureMem, batch);
        }
        else
            m_res.createTextureImage(mesh.material.texturePath, mesh.material.embeddedTexture,
                                     obj.textureImage, obj.textureMem, batch, &albedoFmt);
        m_res.createTextureImageView(obj.textureImage, obj.textureView, albedoFmt);
        obj.sampler = m_res.sharedMaterialSampler();

        VkFormat normalFmt = VK_FORMAT_R8G8B8A8_UNORM;
        if (const DecodedImage* normal = findSlot(DecodedImage::Normal))
        {
            normalFmt = resolveSrgb(TextureKind::Normal, normal->colorSpace)
                            ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
            m_res.createMaterialImageFromPixels(normal->pixels.data(),
                                                (uint32_t)normal->w, (uint32_t)normal->h, normalFmt,
                                                normal->mips.data(), normal->mips.size(),
                                                obj.normalImage, obj.normalMem, batch);
        }
        else
            m_res.createNormalMapImage(mesh.material.normalMapPath, mesh.material.embeddedNormalMap,
                                       obj.normalImage, obj.normalMem, batch, &normalFmt);
        m_res.createTextureImageView(obj.normalImage, obj.normalView, normalFmt);
        obj.normalSampler = m_res.sharedMaterialSampler();

        VkFormat ormFmt = VK_FORMAT_R8G8B8A8_UNORM;
        if (const DecodedImage* orm = findSlot(DecodedImage::ORM))
        {
            ormFmt = resolveSrgb(TextureKind::Orm, orm->colorSpace)
                         ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
            m_res.createMaterialImageFromPixels(orm->pixels.data(),
                                                (uint32_t)orm->w, (uint32_t)orm->h, ormFmt,
                                                orm->mips.data(), orm->mips.size(),
                                                obj.ormImage, obj.ormMem, batch);
            obj.hasOrmMap = true;
        }
        else if (!mesh.material.metallicRoughnessPath.empty()
                 || !mesh.material.embeddedMetallicRoughness.empty())
        {
            m_res.createNormalMapImage(mesh.material.metallicRoughnessPath,
                                       mesh.material.embeddedMetallicRoughness,
                                       obj.ormImage, obj.ormMem, batch, &ormFmt);
            obj.hasOrmMap = true;
        }
        else
        {
            // The shared white one, borrowed: without ORM the shader multiplies by 1
            // and that image is identical in all the meshes. createSolidColorImage
            // stays for whoever DOES want its own (UiSpriteBatch destroys it).
            m_res.sharedWhiteOrm(obj.ormImage, obj.ormMem);
            obj.hasOrmMap = false;
        }
        m_res.createTextureImageView(obj.ormImage, obj.ormView, ormFmt);
        obj.ormSampler = m_res.sharedMaterialSampler();
    }

    int Renderer::addStaticMesh(const Mesh& mesh, const std::vector<DecodedImage>* decoded)
    {
        if (!m_pendingBatch)
            m_pendingBatch = std::make_unique<TransferBatch>(m_gpu);

        // Recycled slot if there is one, and if not the vector grows as usual. Without
        // this, each Play/Stop cycle left one more dead entry (H19): the
        // indices are noted in each GameObject, so compacting is not
        // an option and the only way out is reuse.
        const int slot = m_staticSlots.acquire();
        if (slot < 0)
            m_objects.emplace_back();
        else
            m_objects[(size_t)slot] = RenderObject{};   // no leftovers from the previous one
        const int index = slot < 0 ? (int)m_objects.size() - 1 : slot;
        RenderObject& obj = m_objects[(size_t)index];
        const bool created = buildRenderObject(mesh, obj, m_pendingBatch.get(), decoded);

        if (created)
        {
            SharedGpuMesh& gpu = *m_sharedMeshes.get(obj.sharedIndex);
            allocateObjectDescriptorSet(gpu);
            // The entry is not drawn until this batch's fence signals.
            // It is the spec's product decision: no placeholders, the
            // GameObject appears when it is ready. If the entry already existed its
            // ticket is not touched: either it is already uploaded (0), or it is still waiting for
            // its own, and overwriting it with a later one would delay it more than needed.
            gpu.uploadTicket = m_nextUploadTicket;
        }
        return index;
    }

    void Renderer::destroySharedGpuMesh(const SharedGpuMesh& obj)
    {
        // The sampler is BORROWED (GpuResources::sharedMaterialSampler): a single one
        // for the whole engine. Destroying it here would be a double free as soon as
        // the second material was released.
        vkDestroyImageView(m_gpu.device(), obj.ormView,       nullptr);
        m_res.releaseMaterialImage(obj.ormImage, obj.ormMem);
        // The sampler is BORROWED (GpuResources::sharedMaterialSampler): a single one
        // for the whole engine. Destroying it here would be a double free as soon as
        // the second material was released.
        vkDestroyImageView(m_gpu.device(), obj.normalView,    nullptr);
        m_res.releaseMaterialImage(obj.normalImage, obj.normalMem);
        // The sampler is BORROWED (GpuResources::sharedMaterialSampler): a single one
        // for the whole engine. Destroying it here would be a double free as soon as
        // the second material was released.
        vkDestroyImageView(m_gpu.device(), obj.textureView,   nullptr);
        m_res.releaseMaterialImage(obj.textureImage, obj.textureMem);
        vkDestroyBuffer(m_gpu.device(),    obj.indexBuffer,   nullptr);
        vkFreeMemory(m_gpu.device(),       obj.indexMemory,   nullptr);
        vkDestroyBuffer(m_gpu.device(),    obj.vertexBuffer,  nullptr);
        vkFreeMemory(m_gpu.device(),       obj.vertexMemory,  nullptr);

        // The sets go back to the pool (created with FREE_DESCRIPTOR_SET_BIT), just
        // like destroySkinnedRenderObject: without this, each removeStaticObject /
        // rebuild exhausted its pool instead of recycling its sets. There is no need to
        // null them: obj is always the copy the cache took out of the
        // table, and the original slot was already left empty.
        if (obj.descriptorSets[0] != VK_NULL_HANDLE && obj.descPool != VK_NULL_HANDLE)
            vkFreeDescriptorSets(m_gpu.device(), obj.descPool, MAX_FRAMES, obj.descriptorSets);
    }

    void Renderer::recordShadowPass(VkCommandBuffer cmd)
    {
        // Without lights there are no matrices to extract (computeCascades leaves the
        // identity, whose frustum is the unit cube and would cull almost everything). Even
        // so the N render passes have to be opened: they are what clears the layers and
        // leaves them in DEPTH_STENCIL_READ_ONLY_OPTIMAL, which is the layout the
        // descriptor sets declare. What is skipped is the geometry, which
        // nobody is going to sample (numLights = 0 turns the shadow off in the shader).
        // How many layers are drawn depends on the key light's TYPE: 4 with the
        // cascades of a directional, 1 with the perspective face of a spot,
        // 6 with the cubemap of a point one. The spare ones are opened all the same (the
        // render pass is what clears them and leaves them in the layout the descriptor
        // sets declare) but drawing into them would be recording with the identity
        // matrix onto layers nobody samples.
        for (uint32_t cascade = 0; cascade < SHADOW_MATRICES; cascade++)
        {
            // Drawing happens in the key light's layers (from 0) and in those of the
            // secondary spots (from SHADOW_KEY_MATRICES). The ones in between,
            // which are spare when the key does not use all six, are opened all the same so that
            // the render pass clears them and leaves them in the layout the
            // descriptor sets declare, but nothing is drawn in them.
            const bool esDeLaKey    = cascade < m_shadowPass.activeLayers();
            const bool esDeUnExtra  = cascade >= SHADOW_KEY_MATRICES &&
                                      cascade <  SHADOW_KEY_MATRICES + m_shadowPass.extraLayers();
            const bool drawCasters  = !m_lights.empty() && (esDeLaKey || esDeUnExtra);

            // Render pass, viewport, scissor, pipeline and index push: the pass's.
            // The draws down here are the Renderer's.
            m_shadowPass.beginCascade(cmd, cascade);

            if (!drawCasters)
            {
                m_shadowPass.endCascade(cmd);
                continue;
            }

            // Culling by THIS cascade's frustum, not the camera's nor
            // the larger cascade's: an object the camera does not see can
            // still cast a shadow on what is seen, and an object that
            // falls in the far cascade paints nothing in the near one's map.
            const Frustum lightFrustum = frustumFromViewProj(m_shadowPass.cascadeMatrix(cascade));

            // Same per-object guards as the main pass, with the light's frustum.
            // The grouping is independent of the camera's: the visible
            // sets do not match, so each pass writes its own range of the SSBO
            // (the cascades go first, one after another, and the scene's at the end).
            gatherAndBatch(lightFrustum, /*colorPass*/ false);

            const VkDescriptorSet instSet = m_instanceBuffers.set(m_currentFrame);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_shadowPass.pipelineLayout(),
                1, 1, &instSet, 0, nullptr);

            for (const InstanceBatch& batch : m_instanceBatches)
            {
                const SharedGpuMesh* gpu = m_sharedMeshes.get(batch.sharedIndex);
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_shadowPass.pipelineLayout(), 0, 1, &gpu->descriptorSets[m_currentFrame], 0, nullptr);

                VkBuffer vb[] = { gpu->vertexBuffer };
                VkDeviceSize offsets[] = { 0 };
                vkCmdBindVertexBuffers(cmd, 0, 1, vb, offsets);
                vkCmdBindIndexBuffer(cmd, gpu->indexBuffer, 0, VK_INDEX_TYPE_UINT32);
                vkCmdDrawIndexed(cmd, gpu->indexCount, batch.instanceCount, 0, 0, batch.firstInstance);
            }

            // Skinned. SkinningPass::record runs right before in this same command
            // buffer and leaves outputVertexBuffer with THIS frame's pose and a
            // compute -> VERTEX_INPUT barrier, so here it can already be read as a
            // vertex buffer. Same shader, same layout, same instance SSBO and
            // same cascade matrix as the static ones: the only own thing
            // is the pipeline with the SkinnedVertex stride.
            bool skinnedBound = false;
            for (size_t si = 0; si < m_skinnedObjects.size(); si++)
            {
                // Same visible list the compute consumed: an object
                // that was not dispatched skinning would be left with the pose of the
                // last frame in which it was visible, so its shadow would be
                // wrong. The price is that a character off camera casts no shadow.
                if (si >= m_skinnedVisible.size() || !m_skinnedVisible[si]) continue;
                const SkinnedRenderObject& sobj = m_skinnedObjects[si];
                // "Visible" checkbox of the Mesh component: hidden casts no shadow.
                if (!sobj.meshVisible) continue;
                if (sobj.outputVertexBuffer == VK_NULL_HANDLE || sobj.matGfx.empty()) continue;
                // No room in this frame's instance SSBO: better no
                // shadow than overwriting another pass's range. With the capacity properly
                // sized this should no longer happen; the counter is there
                // so that, if it does, it shows in the PerformancePanel instead of
                // the shadow being lost silently (H23).
                //
                // shadow.vert takes the model matrix from the SSBO via gl_InstanceIndex:
                // one entry per object and a one-instance draw pointing at
                // it with firstInstance. With no room there is no pointer, so there
                // is no way to write without having looked.
                uint32_t instanceIndex = 0;
                glm::mat4* slot = m_instanceBuffers.cur().alloc(1, &instanceIndex);
                if (!slot) { ++m_statInstanceOverflow; break; }
                *slot = sobj.transform;

                if (!skinnedBound)
                {
                    // The layout is the same, so the cascade's push constant
                    // survives the pipeline change; the pass
                    // rewrites it so as not to depend on that compatibility.
                    m_shadowPass.bindSkinnedPipeline(cmd, cascade);
                    skinnedBound = true;
                }

                // Set 0 only for the cascade's UBO (binding 0): shadow.vert does not
                // sample anything, so any material descriptor set
                // works as long as it is of the layout the pipeline declares.
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_shadowPass.pipelineLayout(),
                    0, 1, &sobj.matGfx[0].descSets[m_currentFrame], 0, nullptr);

                VkBuffer svb[] = { sobj.outputVertexBuffer };
                VkDeviceSize soffsets[] = { 0 };
                vkCmdBindVertexBuffers(cmd, 0, 1, svb, soffsets);
                vkCmdBindIndexBuffer(cmd, sobj.indexBuffer, 0, VK_INDEX_TYPE_UINT32);
                for (const auto& sm : sobj.subMeshes)
                    vkCmdDrawIndexed(cmd, sm.indexCount, 1, sm.indexStart, 0, instanceIndex);
            }

            m_shadowPass.endCascade(cmd);
        }
    }

    // The four GRAPHICS pipelines of the skinned meshes. They share
    // shaders with the static ones and only the vertex input changes (stride 80,
    // the skinning compute's output). They are redone when MSAA changes, which
    // is what separates them from the three compute ones of SkinningPass.
    void Renderer::createSkinnedGraphicsPipelines()
    {
         // --- Skinned graphics pipeline (stride=80, same shaders) ---
        {
            auto vertCode = loadShaderFile("shaders/triangle.vert.spv");
            auto fragCode = loadShaderFile("shaders/pbr.frag.spv");
            auto vertMod  = createShaderModule(vertCode);
            auto fragMod  = createShaderModule(fragCode);

            VkPipelineShaderStageCreateInfo stages[2]{};
            stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
            stages[0].module = vertMod; stages[0].pName = "main";
            stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
            stages[1].module = fragMod; stages[1].pName = "main";

            VkVertexInputBindingDescription binding{};
            binding.binding   = 0;
            binding.stride    = 5 * (uint32_t)sizeof(glm::vec4);  // 80 bytes
            binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

            // OutputVertex: pos@0, color@16, uv@32, normal@48, tangent@64
            VkVertexInputAttributeDescription attrs[5]{};
            attrs[0] = { 0, 0, VK_FORMAT_R32G32B32_SFLOAT,  0 };
            attrs[1] = { 1, 0, VK_FORMAT_R32G32B32_SFLOAT, 16 };
            attrs[2] = { 2, 0, VK_FORMAT_R32G32_SFLOAT,    32 };
            attrs[3] = { 3, 0, VK_FORMAT_R32G32B32_SFLOAT, 48 };
            attrs[4] = { 4, 0, VK_FORMAT_R32G32B32_SFLOAT, 64 };

            VkPipelineVertexInputStateCreateInfo vi{};
            vi.sType                           = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
            vi.vertexBindingDescriptionCount   = 1;  vi.pVertexBindingDescriptions  = &binding;
            vi.vertexAttributeDescriptionCount = 5;  vi.pVertexAttributeDescriptions = attrs;

            // The SAME state as the static ones, not a single different value: it lives in
            // PipelineDefaults.h since H10. The only thing that really separates this
            // pipeline from the one above is the vertex input (the skinning compute's
            // output, stride 80) and its shaders.
            const GraphicsPipelineState estadoComun(m_aaSampleCount);

            VkGraphicsPipelineCreateInfo pci{};
            estadoComun.fill(pci);
            pci.stageCount          = 2;
            pci.pStages             = stages;
            pci.pVertexInputState   = &vi;
            pci.layout              = m_pipelineLayout;
            pci.renderPass          = m_offscreenRenderPass;

            if (vkCreateGraphicsPipelines(m_gpu.device(), VK_NULL_HANDLE, 1, &pci, nullptr, &m_skinnedGfxPipeline) != VK_SUCCESS)
                throw std::runtime_error("failed to create skinned graphics pipeline!");

            // Skinned wireframe pipeline: same vertex input/layout as the
            // gfx pipeline above, only polygonMode changes to LINE and the
            // fragment shader to flat color.
            auto wireFragCode = loadShaderFile("shaders/wireframe.frag.spv");
            auto wireFragMod  = createShaderModule(wireFragCode);

            VkPipelineShaderStageCreateInfo wireFragStage{};
            wireFragStage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            wireFragStage.stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
            wireFragStage.module = wireFragMod;
            wireFragStage.pName  = "main";

            VkPipelineShaderStageCreateInfo wireStages[] = { stages[0], wireFragStage };

            VkPipelineRasterizationStateCreateInfo wireRs = estadoComun.rasterization;
            wireRs.polygonMode = VK_POLYGON_MODE_LINE;

            VkGraphicsPipelineCreateInfo wirePci = pci;
            wirePci.pStages             = wireStages;
            wirePci.pRasterizationState = &wireRs;

            if (vkCreateGraphicsPipelines(m_gpu.device(), VK_NULL_HANDLE, 1, &wirePci, nullptr, &m_skinnedWireframePipeline) != VK_SUCCESS)
                throw std::runtime_error("failed to create skinned wireframe pipeline!");

            // Skinned selection outline: twin of the static one in
            // createPipeline (same shaders, cullMode FRONT), but over this
            // vertex input of stride 80. The buffer it reads is the skinning
            // compute's OUTPUT, so the hull is extruded over this frame's already
            // deformed pose, not over the rest one.
            // pos@0 and normal@48 of OutputVertex, the skinning compute's
            // output: the hull is extruded over THIS frame's deformed pose,
            // not over the rest one.
            m_outlinePass.createSkinnedPipelines(outlineCtx(), pci, estadoComun.rasterization, vi, 0, 48);

            vkDestroyShaderModule(m_gpu.device(), vertMod, nullptr);
            vkDestroyShaderModule(m_gpu.device(), fragMod, nullptr);
            vkDestroyShaderModule(m_gpu.device(), wireFragMod, nullptr);
        }
    }

    void Renderer::destroySkinnedGeometry(const SkinnedGeometry& g)
    {
        const VkBuffer       bufs[] = { g.posBuf, g.rotBuf, g.scaleBuf, g.boneBuf, g.vtxBuf, g.idxBuf };
        const VkDeviceMemory mems[] = { g.posMem, g.rotMem, g.scaleMem, g.boneMem, g.vtxMem, g.idxMem };
        for (VkBuffer b : bufs)       if (b != VK_NULL_HANDLE) vkDestroyBuffer(m_gpu.device(), b, nullptr);
        for (VkDeviceMemory m : mems) if (m != VK_NULL_HANDLE) vkFreeMemory(m_gpu.device(), m, nullptr);
    }

    void Renderer::destroySkinnedRenderObject(SkinnedRenderObject& obj)
    {
        auto destroy = [&](VkBuffer& b, VkDeviceMemory& m)
        {
            if (b != VK_NULL_HANDLE) { vkDestroyBuffer(m_gpu.device(), b, nullptr); b = VK_NULL_HANDLE; }
            if (m != VK_NULL_HANDLE) { vkFreeMemory(m_gpu.device(), m, nullptr);    m = VK_NULL_HANDLE; }
        };
        // Shared read-only buffers (B6): the last character that uses them
        // destroys them. One not in the cache (never happens today: acquire
        // always caches a non-empty key) is destroyed as its own.
        {
            SkinnedGeometry g;
            g.posBuf   = obj.keyframePosBuffer;   g.posMem   = obj.keyframePosMemory;
            g.rotBuf   = obj.keyframeRotBuffer;   g.rotMem   = obj.keyframeRotMemory;
            g.scaleBuf = obj.keyframeScaleBuffer; g.scaleMem = obj.keyframeScaleMemory;
            g.boneBuf  = obj.boneInfoBuffer;      g.boneMem  = obj.boneInfoMemory;
            g.vtxBuf   = obj.inputVertexBuffer;   g.vtxMem   = obj.inputVertexMemory;
            g.idxBuf   = obj.indexBuffer;         g.idxMem   = obj.indexMemory;
            if (m_skinnedGeometry.contains(g))
                m_skinnedGeometry.release(g, [&](const SkinnedGeometry& h) { destroySkinnedGeometry(h); });
            else if (!(g == SkinnedGeometry{}))
                destroySkinnedGeometry(g);
            obj.keyframePosBuffer   = VK_NULL_HANDLE; obj.keyframePosMemory   = VK_NULL_HANDLE;
            obj.keyframeRotBuffer   = VK_NULL_HANDLE; obj.keyframeRotMemory   = VK_NULL_HANDLE;
            obj.keyframeScaleBuffer = VK_NULL_HANDLE; obj.keyframeScaleMemory = VK_NULL_HANDLE;
            obj.boneInfoBuffer      = VK_NULL_HANDLE; obj.boneInfoMemory      = VK_NULL_HANDLE;
            obj.inputVertexBuffer   = VK_NULL_HANDLE; obj.inputVertexMemory   = VK_NULL_HANDLE;
            obj.indexBuffer         = VK_NULL_HANDLE; obj.indexMemory         = VK_NULL_HANDLE;
        }
        destroy(obj.localTransformBuffer, obj.localTransformMemory);
        destroy(obj.finalBoneBuffer,      obj.finalBoneMemory);
        destroy(obj.poseTrsBuffer,        obj.poseTrsMemory);
        destroy(obj.frozenTrsBuffer,      obj.frozenTrsMemory);
        if (obj.poseBlockMapped)
        {
            vkUnmapMemory(m_gpu.device(), obj.poseBlockMemory);
            obj.poseBlockMapped = nullptr;
        }
        destroy(obj.poseBlockBuffer,      obj.poseBlockMemory);
        if (obj.ikBlockMapped)
        {
            vkUnmapMemory(m_gpu.device(), obj.ikBlockMemory);
            obj.ikBlockMapped = nullptr;
        }
        destroy(obj.ikBlockBuffer,        obj.ikBlockMemory);
        destroy(obj.outputVertexBuffer,   obj.outputVertexMemory);

        // Shared: it is destroyed when the last character releases it. If it is not
        // from the cache (the white fill-in), releaseMaterialImage knows it is
        // borrowed and does not destroy it.
        auto soltarMaterial = [&](VkImage img, VkDeviceMemory mem) {
            const MaterialImage m{ img, mem };
            if (m_skinnedTextures.contains(m))
                m_skinnedTextures.release(m, [&](const MaterialImage& h) { m_res.releaseMaterialImage(h.image, h.mem); });
            else
                m_res.releaseMaterialImage(img, mem);
        };
        for (auto& mgfx : obj.matGfx)
        {
            // The sampler is BORROWED (GpuResources::sharedMaterialSampler): a single one
            // for the whole engine. Destroying it here would be a double free as soon as
            // the second material was released.
            if (mgfx.ormView       != VK_NULL_HANDLE) { vkDestroyImageView(m_gpu.device(), mgfx.ormView,       nullptr); }
            soltarMaterial(mgfx.ormImage, mgfx.ormMem);
            // The sampler is BORROWED (GpuResources::sharedMaterialSampler): a single one
            // for the whole engine. Destroying it here would be a double free as soon as
            // the second material was released.
            if (mgfx.normalView    != VK_NULL_HANDLE) { vkDestroyImageView(m_gpu.device(), mgfx.normalView,    nullptr); }
            soltarMaterial(mgfx.normalImage, mgfx.normalMem);
            // The sampler is BORROWED (GpuResources::sharedMaterialSampler): a single one
            // for the whole engine. Destroying it here would be a double free as soon as
            // the second material was released.
            if (mgfx.textureView   != VK_NULL_HANDLE) { vkDestroyImageView(m_gpu.device(), mgfx.textureView,   nullptr); }
            soltarMaterial(mgfx.textureImage, mgfx.textureMem);
        }

        // The sets go back to the pool (created with FREE_DESCRIPTOR_SET_BIT): without
        // this, rebuilding the object would exhaust it.
        if (obj.computeDescSet != VK_NULL_HANDLE && obj.computeDescPool != VK_NULL_HANDLE)
        {
            vkFreeDescriptorSets(m_gpu.device(), obj.computeDescPool, 1, &obj.computeDescSet);
            obj.computeDescSet  = VK_NULL_HANDLE;
            obj.computeDescPool = VK_NULL_HANDLE;
        }
        for (auto& mgfx : obj.matGfx)
        {
            if (mgfx.descSets[0] != VK_NULL_HANDLE && mgfx.descPool != VK_NULL_HANDLE)
                vkFreeDescriptorSets(m_gpu.device(), mgfx.descPool, MAX_FRAMES, mgfx.descSets);
            mgfx.descSets[0] = VK_NULL_HANDLE;
            mgfx.descSets[1] = VK_NULL_HANDLE;
            mgfx.descPool    = VK_NULL_HANDLE;
        }

        obj.matGfx.clear();
        obj.subMeshes.clear();
    }

    int Renderer::addSkinnedMesh(const SkinnedMesh& mesh, const std::vector<DecodedImage>* decoded)
    {
        if (!m_pendingBatch)
            m_pendingBatch = std::make_unique<TransferBatch>(m_gpu);

        // Same recycling as in addStaticMesh.
        const int slot = m_skinnedSlots.acquire();
        if (slot < 0)
            m_skinnedObjects.emplace_back();
        else
            m_skinnedObjects[(size_t)slot] = SkinnedRenderObject{};
        const int index = slot < 0 ? (int)m_skinnedObjects.size() - 1 : slot;
        SkinnedRenderObject& obj = m_skinnedObjects[(size_t)index];
        initSkinnedRenderObject(obj, mesh, m_pendingBatch.get(), decoded);

        // Same as the static ones: invisible until the batch's fence signals.
        obj.uploadTicket = m_nextUploadTicket;
        return index;
    }

    void Renderer::initSkinnedRenderObject(SkinnedRenderObject& obj, const SkinnedMesh& mesh,
                                           TransferBatch* batch,
                                           const std::vector<DecodedImage>* decoded)
    {
        // The textures of skinned materials are not decoded in the worker
        // (there is no way to map a DecodedImage to a specific submesh), so they
        // always take the synchronous stbi_load path, but ALWAYS with a batch,
        // so that their uploads land in m_pendingBatch and the ticket gets resolved.
        (void)decoded;
        const Skeleton&      skel = mesh.skeleton;
        // Clip 0 for the object's duration/ticksPerSecond: they are what
        // updateAnimation() consumes, which only runs in the case WITHOUT an Animator (Task 3
        // adds the Animator path). Mesh without animations -> empty clip.
        static const AnimationClip kEmptyClip{};
        const AnimationClip& clip = mesh.animationClips.empty() ? kEmptyClip : mesh.animationClips[0];
        int boneCount   = (int)skel.names.size();
        int vertexCount = (int)mesh.skinnedVertices.size();

        obj.name           = mesh.name;
        obj.boneCount      = (uint32_t)boneCount;
        obj.vertexCount    = (uint32_t)vertexCount;
        obj.indexCount     = (uint32_t)mesh.indices.size();
        obj.duration       = clip.duration;
        obj.ticksPerSecond = (clip.ticksPerSecond > 0.0f) ? clip.ticksPerSecond : 24.0f;
        // Culling bound: it is computed ONCE on load because it depends only on
        // the mesh and its clips, not on the pose or the transform.
        obj.boundRadius    = skinnedBoundRadius(mesh);
        obj.hasBounds      = obj.boundRadius > 0.0f;
        // Size of the mesh at rest, ONLY to scale the outline thickness.
        // boundRadius is no good: that bound holds for any pose of any
        // clip and is loose on purpose (an arm that reaches far inflates it
        // several times over the body), so using it gave a disproportionate
        // border precisely on the meshes with animation.
        obj.restMaxExtent = 0.0f;
        if (!mesh.skinnedVertices.empty())
        {
            glm::vec3 bMin = glm::vec3(mesh.skinnedVertices[0].position);
            glm::vec3 bMax = bMin;
            for (const auto& v : mesh.skinnedVertices)
            {
                bMin = glm::min(bMin, glm::vec3(v.position));
                bMax = glm::max(bMax, glm::vec3(v.position));
            }
            const glm::vec3 e = bMax - bMin;
            obj.restMaxExtent = glm::max(e.x, glm::max(e.y, e.z));
        }

        // --- Flatten keyframes of ALL the clips into GPU format ---
        // (packSkinnedClips lives outside so it can be tested without a VkDevice)
        const PackedClips packed = packSkinnedClips(mesh);
        obj.clipCount = skinnedClipCount(mesh);

        // --- Read-only SSBOs + index buffer: shared among clones (B6) ---
        // A later character reusing buffers of an earlier batch is safe: its
        // uploadTicket is later, so it is not drawn before that batch lands.
        const SkinnedGeometry geo = m_skinnedGeometry.acquire(skinnedGeometryKey(mesh, packed), [&] {
            SkinnedGeometry g;
            // Half-created (VRAM exhausted): nothing is in obj yet, so without
            // this the buffers already made would leak.
            try {
            m_res.uploadBuffer(packed.pos.data(),   packed.pos.size()   * sizeof(GpuPosKey),
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, g.posBuf,   g.posMem,   batch);
            m_res.uploadBuffer(packed.rot.data(),   packed.rot.size()   * sizeof(GpuRotKey),
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, g.rotBuf,   g.rotMem,   batch);
            m_res.uploadBuffer(packed.scale.data(), packed.scale.size() * sizeof(GpuPosKey),
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, g.scaleBuf, g.scaleMem, batch);
            m_res.uploadBuffer(packed.boneInfos.data(), packed.boneInfos.size() * sizeof(GpuBoneInfo),
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, g.boneBuf,  g.boneMem,  batch);
            m_res.uploadBuffer(mesh.skinnedVertices.data(), mesh.skinnedVertices.size() * sizeof(SkinnedVertex),
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, g.vtxBuf,   g.vtxMem,   batch);
            createIndexBuffer(mesh.indices, g.idxBuf, g.idxMem, batch);
            } catch (...) {
                // With a batch, the copies into these buffers are already
                // recorded: the batch frees them after its fence, not now.
                if (batch)
                {
                    const std::pair<VkBuffer, VkDeviceMemory> pares[] = {
                        {g.posBuf, g.posMem}, {g.rotBuf, g.rotMem}, {g.scaleBuf, g.scaleMem},
                        {g.boneBuf, g.boneMem}, {g.vtxBuf, g.vtxMem}, {g.idxBuf, g.idxMem} };
                    for (const auto& [b, m] : pares)
                        if (b != VK_NULL_HANDLE || m != VK_NULL_HANDLE) batch->addStaging(b, m);
                }
                else
                    destroySkinnedGeometry(g);
                throw;
            }
            return g;
        });
        obj.keyframePosBuffer   = geo.posBuf;   obj.keyframePosMemory   = geo.posMem;
        obj.keyframeRotBuffer   = geo.rotBuf;   obj.keyframeRotMemory   = geo.rotMem;
        obj.keyframeScaleBuffer = geo.scaleBuf; obj.keyframeScaleMemory = geo.scaleMem;
        obj.boneInfoBuffer      = geo.boneBuf;  obj.boneInfoMemory      = geo.boneMem;
        obj.inputVertexBuffer   = geo.vtxBuf;   obj.inputVertexMemory   = geo.vtxMem;
        obj.indexBuffer         = geo.idxBuf;   obj.indexMemory         = geo.idxMem;

        // --- Dynamic SSBOs (device local, no initial data) ---
        m_res.createBuffer((uint32_t)boneCount * sizeof(glm::mat4),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
            obj.localTransformBuffer, obj.localTransformMemory);

        m_res.createBuffer((uint32_t)boneCount * sizeof(glm::mat4),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
            obj.finalBoneBuffer, obj.finalBoneMemory);

        // TRS pose (bone_eval) and the frozen one: 3 vec4 per bone; they are copied
        // into each other when interrupting a fade, hence the transfer usages.
        for (auto* par : { &obj.poseTrsBuffer, &obj.frozenTrsBuffer })
        {
            VkDeviceMemory& mem = (par == &obj.poseTrsBuffer) ? obj.poseTrsMemory : obj.frozenTrsMemory;
            m_res.createBuffer((uint32_t)boneCount * 3 * sizeof(glm::vec4) * kMaxLayersPose,
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                    VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, *par, mem);
        }

        // Pose block: one copy per in-flight frame, host-visible and
        // mapped forever (it is rewritten every frame in SkinningPass).
        m_res.createBuffer((uint32_t)(MAX_FRAMES * poseBlockUints((uint32_t)boneCount) * sizeof(uint32_t)),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            obj.poseBlockBuffer, obj.poseBlockMemory);
        if (vkMapMemory(m_gpu.device(), obj.poseBlockMemory, 0, VK_WHOLE_SIZE, 0, &obj.poseBlockMapped) != VK_SUCCESS)
            throw std::runtime_error("failed to map the pose block!");

        // IK block: same, one copy per in-flight frame.
        m_res.createBuffer((uint32_t)(MAX_FRAMES * ikBlockUints() * sizeof(uint32_t)),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            obj.ikBlockBuffer, obj.ikBlockMemory);
        if (vkMapMemory(m_gpu.device(), obj.ikBlockMemory, 0, VK_WHOLE_SIZE, 0, &obj.ikBlockMapped) != VK_SUCCESS)
            throw std::runtime_error("failed to map the ik block!");

        // --- Output vertex buffer: SSBO + VB, stride 80 bytes (5×vec4) ---
        constexpr VkDeviceSize OUT_VERT = 5 * sizeof(glm::vec4);
        m_res.createBuffer((uint32_t)vertexCount * OUT_VERT,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
            obj.outputVertexBuffer, obj.outputVertexMemory);

        // --- Compute descriptor set ---
        // The pass chains pools as needed, so this no longer breaks
        // with character 17. It remembers which pool it came from: that is what
        // destroySkinnedRenderObject needs to give it back.
        obj.computeDescPool = m_skinningPass.allocateSet(skinningCtx(), obj.computeDescSet);
        if (obj.computeDescPool == VK_NULL_HANDLE)
            throw std::runtime_error("failed to allocate compute descriptor set!");

        VkDescriptorBufferInfo bufInfos[12]{};
        bufInfos[0] = { obj.keyframePosBuffer,    0, VK_WHOLE_SIZE };
        bufInfos[1] = { obj.keyframeRotBuffer,    0, VK_WHOLE_SIZE };
        bufInfos[2] = { obj.keyframeScaleBuffer,  0, VK_WHOLE_SIZE };
        bufInfos[3] = { obj.boneInfoBuffer,       0, VK_WHOLE_SIZE };
        bufInfos[4] = { obj.localTransformBuffer, 0, VK_WHOLE_SIZE };
        bufInfos[5] = { obj.finalBoneBuffer,      0, VK_WHOLE_SIZE };
        bufInfos[6] = { obj.inputVertexBuffer,    0, VK_WHOLE_SIZE };
        bufInfos[7] = { obj.outputVertexBuffer,   0, VK_WHOLE_SIZE };
        bufInfos[8] = { obj.poseTrsBuffer,        0, VK_WHOLE_SIZE };
        bufInfos[9] = { obj.frozenTrsBuffer,      0, VK_WHOLE_SIZE };
        bufInfos[10] = { obj.poseBlockBuffer,     0, VK_WHOLE_SIZE };
        bufInfos[11] = { obj.ikBlockBuffer,       0, VK_WHOLE_SIZE };

        VkWriteDescriptorSet writes[12]{};
        for (int i = 0; i < 12; i++)
        {
            writes[i].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet          = obj.computeDescSet;
            writes[i].dstBinding      = (uint32_t)i;
            writes[i].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[i].descriptorCount = 1;
            writes[i].pBufferInfo     = &bufInfos[i];
        }
        vkUpdateDescriptorSets(m_gpu.device(), 12, writes, 0, nullptr);

        // --- Textures and descriptor sets per material ---
        constexpr uint8_t white[4] = {255, 255, 255, 255};
        obj.matGfx.resize(mesh.materials.size());

        for (size_t mi = 0; mi < mesh.materials.size(); mi++)
        {
            const Material& smat = mesh.materials[mi];
            SkinnedMatGfx& mgfx = obj.matGfx[mi];

            // Color, normal and ORM, shared between characters of the same FBX
            // (m_skinnedTextures). Without a texture, the usual white fill-in,
            // which does not go into the cache.
            // The format the image was created with (resolveSrgb) travels in the
            // cache entry: whoever shares it declares the SAME view.
            auto pedir = [&](const std::string& ruta, const std::vector<uint8_t>& emb, TextureKind tipo,
                             VkImage& img, VkDeviceMemory& mem, VkFormat& fmt) {
                const MaterialImage m = m_skinnedTextures.acquire(
                    makeTextureKey(ruta, emb, tipo, textureKeySuffix(ruta)), [&] {
                    MaterialImage nueva;
                    if (tipo == TextureKind::BaseColor)
                        m_res.createTextureImage(ruta, emb, nueva.image, nueva.mem, batch, &nueva.format);
                    else
                        m_res.createNormalMapImage(ruta, emb, nueva.image, nueva.mem, batch, &nueva.format);
                    return nueva;
                });
                img = m.image;
                mem = m.mem;
                fmt = m.format;
            };

            // Diffuse
            VkFormat albedoFmt = VK_FORMAT_R8G8B8A8_SRGB;
            pedir(smat.texturePath, smat.embeddedTexture, TextureKind::BaseColor, mgfx.textureImage, mgfx.textureMem, albedoFmt);
            m_res.createTextureImageView(mgfx.textureImage, mgfx.textureView, albedoFmt);
            mgfx.sampler = m_res.sharedMaterialSampler();

            // Normal map
            VkFormat normalFmt = VK_FORMAT_R8G8B8A8_UNORM;
            pedir(smat.normalMapPath, smat.embeddedNormalMap, TextureKind::Normal, mgfx.normalImage, mgfx.normalMem, normalFmt);
            m_res.createTextureImageView(mgfx.normalImage, mgfx.normalView, normalFmt);
            mgfx.normalSampler = m_res.sharedMaterialSampler();

            // ORM
            VkFormat ormFmt = VK_FORMAT_R8G8B8A8_UNORM;
            if (!smat.metallicRoughnessPath.empty() || !smat.embeddedMetallicRoughness.empty())
            {
                pedir(smat.metallicRoughnessPath, smat.embeddedMetallicRoughness, TextureKind::Orm,
                      mgfx.ormImage, mgfx.ormMem, ormFmt);
                mgfx.metallic  = 1.0f;
                mgfx.roughness = 1.0f;
            }
            else
            {
                m_res.sharedWhiteOrm(mgfx.ormImage, mgfx.ormMem);
                mgfx.metallic  = smat.metallic;
                mgfx.roughness = smat.roughness;
            }
            m_res.createTextureImageView(mgfx.ormImage, mgfx.ormView, ormFmt);
            mgfx.ormSampler = m_res.sharedMaterialSampler();

            // Descriptor sets
            mgfx.descPool = allocateSharedSets(mgfx.descSets);
            if (mgfx.descPool == VK_NULL_HANDLE)
                throw std::runtime_error("failed to allocate skinned graphics descriptor sets!");

            for (int fi = 0; fi < MAX_FRAMES; fi++)
            {
                VkDescriptorBufferInfo uboInfo{};
                uboInfo.buffer = m_uniformBuffers[fi];
                uboInfo.offset = 0;
                uboInfo.range  = sizeof(UniformBufferObject);

                VkDescriptorImageInfo texInfo{};
                texInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                texInfo.imageView   = mgfx.textureView;
                texInfo.sampler     = mgfx.sampler;

                VkDescriptorImageInfo nrmInfo{};
                nrmInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                nrmInfo.imageView   = mgfx.normalView;
                nrmInfo.sampler     = mgfx.normalSampler;

                VkDescriptorImageInfo shdInfo{};
                shdInfo.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
                shdInfo.imageView   = m_shadowPass.view();
                shdInfo.sampler     = m_shadowPass.sampler();

                VkDescriptorImageInfo ormInfo{};
                ormInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                ormInfo.imageView   = mgfx.ormView;
                ormInfo.sampler     = mgfx.ormSampler;


                VkWriteDescriptorSet gw[7]{};
                gw[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                gw[0].dstSet = mgfx.descSets[fi]; gw[0].dstBinding = 0;
                gw[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
                gw[0].descriptorCount = 1; gw[0].pBufferInfo = &uboInfo;

                gw[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                gw[1].dstSet = mgfx.descSets[fi]; gw[1].dstBinding = 1;
                gw[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                gw[1].descriptorCount = 1; gw[1].pImageInfo = &texInfo;

                gw[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                gw[2].dstSet = mgfx.descSets[fi]; gw[2].dstBinding = 2;
                gw[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                gw[2].descriptorCount = 1; gw[2].pImageInfo = &nrmInfo;

                gw[3].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                gw[3].dstSet = mgfx.descSets[fi]; gw[3].dstBinding = 3;
                gw[3].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                gw[3].descriptorCount = 1; gw[3].pImageInfo = &shdInfo;

                gw[4].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                gw[4].dstSet = mgfx.descSets[fi]; gw[4].dstBinding = 4;
                gw[4].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                gw[4].descriptorCount = 1; gw[4].pImageInfo = &ormInfo;

                // Same single place as the static meshes.
                VkDescriptorImageInfo iblInfos[2]{};
                IblPass::fillIblWrites(mgfx.descSets[fi], m_iblPass.irradianceView(),
                                       m_iblPass.prefilterView(), m_iblPass.sampler(),
                                       iblInfos, &gw[5]);

                vkUpdateDescriptorSets(m_gpu.device(), 7, gw, 0, nullptr);

                // Binding 7 (SSAO), same as in the static path: it goes apart
                // because its view is redone with the swapchain.
                writeSsaoBinding(mgfx.descSets[fi], fi);
            }
        }

        // --- SubMesh draw list ---
        obj.subMeshes.resize(mesh.subMeshRanges.size());
        for (size_t si = 0; si < mesh.subMeshRanges.size(); si++)
        {
            obj.subMeshes[si].indexStart   = mesh.subMeshRanges[si].indexStart;
            obj.subMeshes[si].indexCount   = mesh.subMeshRanges[si].indexCount;
            obj.subMeshes[si].materialIndex = mesh.subMeshRanges[si].materialIndex;
        }
    }

    void Renderer::rebuildSkinnedMesh(int index, const SkinnedMesh& mesh)
    {
        if (index < 0 || index >= (int)m_skinnedObjects.size()) return;

        // Wait for the GPU to finish: an in-flight command buffer (double
        // buffering) may be reading the buffers we are about to destroy.
        // Same reason as in removeGameObject.
        // Also the uploads not yet submitted: the rebuild runs without a batch
        // (ticket 0, drawn at once), and on a cache hit its shared geometry may
        // be the one an addSkinnedMesh of this frame left in m_pendingBatch (B6).
        flushUploadsAndWait();
        vkDeviceWaitIdle(m_gpu.device());

        SkinnedRenderObject& obj = m_skinnedObjects[index];

        // The animation state belongs to the Animator, not to the buffers: losing it
        // would make the character visibly jump when importing a file.
        const glm::mat4 transform  = obj.transform;
        const float     animTime   = obj.animTime;
        const uint32_t  activeClip = obj.activeClip;

        destroySkinnedRenderObject(obj);
        obj = SkinnedRenderObject{};
        initSkinnedRenderObject(obj, mesh);

        obj.transform = transform;
        obj.animTime  = animTime;
        // Clamp: the clip list may have shrunk and activeClip would point
        // outside the BoneInfos SSBO, with the compute silently reading garbage.
        // Same criterion as setAnimationState.
        obj.activeClip = clampClipIndex(activeClip, obj.clipCount);
    }

    void Renderer::updateAnimation(int index, float deltaTime)
    {
        if (index < 0 || index >= (int)m_skinnedObjects.size()) return;
        auto& obj = m_skinnedObjects[index];
        // The whole rule (pace, wrap and freezing the hidden one) lives in
        // advanceMeshClock, shared with D3D12: written twice, the two
        // copies diverged (A13).
        obj.animTime = advanceMeshClock(obj.animTime, deltaTime, obj.ticksPerSecond,
                                        obj.duration, obj.meshVisible);
    }

    void Renderer::setAnimationState(int index, uint32_t clipIndex, float animTime)
    {
        if (index < 0 || index >= (int)m_skinnedObjects.size()) return;
        auto& obj = m_skinnedObjects[index];
        // Clamp and not assert: an out-of-range clipIndex (a scene with a graph that
        // references a clip the FBX no longer has) would make clipBase point
        // outside the BoneInfos SSBO, and the compute would silently read garbage.
        obj.activeClip = clampClipIndex(clipIndex, obj.clipCount);
        obj.animTime   = animTime;
        // A single sample from here on: an Animator's pose is sent again by
        // setAnimationPose every frame if needed.
        obj.hasPose = false;
    }

    void Renderer::setAnimationIk(int index, const AnimationIk& ik)
    {
        if (index < 0 || index >= (int)m_skinnedObjects.size()) return;
        // The bone indices come from the Animator, which resolved them against the
        // SAME skeleton: only what does not fit in the SSBO is filtered.
        auto& obj = m_skinnedObjects[index];
        obj.ik = ik;
        for (int k = 0; k < obj.ik.count; k++)
            if (obj.ik.solves[k].bone >= (int)obj.boneCount) obj.ik.solves[k].bone = -1;
    }

    void Renderer::setAnimationPose(int index, const AnimationPose& pose)
    {
        if (index < 0 || index >= (int)m_skinnedObjects.size()) return;
        auto& obj = m_skinnedObjects[index];
        obj.pose    = pose;
        obj.hasPose = true;
        // Each layer's mask is copied: the Animator's is only valid during
        // this call. SkinningPass re-points the pose at these copies.
        for (int L = 0; L < kMaxLayersPose; L++)
        {
            const std::vector<uint8_t>* m = (L < pose.layerCount) ? pose.layers[L].mask : nullptr;
            if (m) obj.poseMasks[L] = *m; else obj.poseMasks[L].clear();
            obj.pose.layers[L].mask = nullptr;
        }
        // Same clamp as the active clip: each clip indexes the BoneInfos SSBO
        // and an out-of-range one would silently read garbage.
        int masPesada = -1;
        for (int k = 0; k < obj.pose.count; k++)
        {
            obj.pose.samples[k].clip = (int)clampClipIndex((uint32_t)std::max(0, obj.pose.samples[k].clip), obj.clipCount);
            if (masPesada < 0 || obj.pose.samples[k].weight > obj.pose.samples[masPesada].weight) masPesada = k;
        }
        // The rest of the renderer (bounds, outline) still looks at a single clip:
        // the one with the most weight.
        if (masPesada >= 0)
        {
            obj.activeClip = (uint32_t)obj.pose.samples[masPesada].clip;
            obj.animTime   = obj.pose.samples[masPesada].time;
        }
    }

    void Renderer::setSkinnedTransform(int index, const glm::mat4& t)
    {
        if (index >= 0 && index < (int)m_skinnedObjects.size())
            m_skinnedObjects[index].transform = t;
    }

    void Renderer::flushPendingUploads()
    {
        if (m_pendingBatch && !m_pendingBatch->empty())
        {
            m_pendingBatch->submit();
            m_inFlightBatches.push_back(InFlightBatch{m_nextUploadTicket,
                                                      std::move(m_pendingBatch)});
        }
        m_pendingBatch.reset();
        ++m_nextUploadTicket;
    }

    bool Renderer::hasPendingUploads() const
    {
        return (m_pendingBatch && !m_pendingBatch->empty()) || !m_inFlightBatches.empty();
    }

    void Renderer::flushUploadsAndWait()
    {
        // Submits the pending batch (if any) and moves it to m_inFlightBatches.
        flushPendingUploads();
        // After vkDeviceWaitIdle all the in-flight batches' fences are
        // signaled, so each one is complete() and tickDeferredDeletes
        // reclaims them and advances m_lastCompletedTicket in a single pass: the loop
        // ends as soon as hasPendingUploads() is false. The wait is a
        // deliberate stall, acceptable in these rare synchronous transitions.
        vkDeviceWaitIdle(m_gpu.device());
        while (hasPendingUploads())
            tickDeferredDeletes();
    }

    void Renderer::tickDeferredDeletes()
    {
        m_deferredDeletes.tick(m_gpu.device());

        // The batches are reclaimed in strict ORDER: a ticket is only considered
        // complete when its own and all the earlier ones have signaled. That is why
        // we stop at the first batch at the front that has not signaled yet, even if a
        // later one already has: if we let a later batch advance
        // m_lastCompletedTicket, an object from an earlier batch still in flight
        // (textures still in TRANSFER_DST_OPTIMAL) would become visible and would sample
        // garbage. Batches are inserted with an increasing ticket, so the front
        // is always the oldest.
        while (!m_inFlightBatches.empty() && m_inFlightBatches.front().batch->complete())
        {
            m_inFlightBatches.front().batch->reclaim();
            m_lastCompletedTicket = m_inFlightBatches.front().ticket;
            m_inFlightBatches.erase(m_inFlightBatches.begin());
        }
    }

    void Renderer::releaseRenderObject(RenderObject& obj)
    {
        // The object drops its reference NOW: the rest of the frame sees it without
        // resources (the skip in recordCommandBuffer detects it through
        // sharedIndex < 0). If more holders remain nothing is destroyed; if it was
        // the last one, the cache hands us a copy of the handles and the
        // real destruction happens kDelayFrames later.
        const int index = obj.sharedIndex;
        obj.sharedIndex = -1;

        m_sharedMeshes.release(index, [this](const SharedGpuMesh& gpu) {
            m_deferredDeletes.push([this, gpu](VkDevice) {
                destroySharedGpuMesh(gpu);
            });
        });
    }

    void Renderer::queueDestroySkinnedRenderObject(SkinnedRenderObject& obj)
    {
        SkinnedRenderObject snapshot = std::move(obj);
        obj = SkinnedRenderObject{};

        m_deferredDeletes.push([this, snapshot = std::move(snapshot)](VkDevice) mutable {
            destroySkinnedRenderObject(snapshot);
        });
    }

    void Renderer::removeStaticObject(int index)
    {
        if (index < 0 || index >= (int)m_objects.size()) return;
        RenderObject& obj = m_objects[index];
        if (obj.sharedIndex < 0) return; // already released
        releaseRenderObject(obj);
        // The entry is left empty and its slot goes back to the pool. The GPU resources
        // are released by the deferred queue kDelayFrames later, but the ENTRY
        // has nothing left: releaseRenderObject left sharedIndex at -1, which is
        // what the skip in recordCommandBuffer looks at.
        m_staticSlots.release(index);
    }

    void Renderer::removeSkinnedObject(int index)
    {
        if (index < 0 || index >= (int)m_skinnedObjects.size()) return;
        SkinnedRenderObject& obj = m_skinnedObjects[index];
        if (obj.outputVertexBuffer == VK_NULL_HANDLE) return; // already released
        // queueDestroy... already leaves obj empty: the assignment afterwards is redundant and
        // would overwrite the snapshot if left in.
        queueDestroySkinnedRenderObject(obj);
        m_skinnedSlots.release(index);
    }

    void Renderer::removeGameObject(GameObject* node)
    {
        if (!node) return;
        // No vkDeviceWaitIdle: removeStaticObject/removeSkinnedObject enqueue
        // the destruction kDelayFrames frames out, which is longer than any
        // in-flight command buffer can take.
        node->traverse([this](GameObject* go) {
            // And the indices to -1 in the SAME operation that releases the slot:
            // leaving them pointing at an already released slot cannot survive
            // this call. Since slots are recycled, a stale index
            // does not point at an empty slot but at the NEXT object that
            // takes it, and all the readers take it as good just by checking that
            // it is >= 0.
            //
            // The caller used to do it, and the DirectX 12 backend already did it here
            // (D3D12Renderer::removeGameObject): the two backends diverged on
            // this, with Vulkan depending on four places remembering.
            if (go->staticRenderIndex >= 0)
                removeStaticObject(go->staticRenderIndex);
            go->staticRenderIndex = -1;
            if (go->skinnedRenderIndex >= 0)
                removeSkinnedObject(go->skinnedRenderIndex);
            go->skinnedRenderIndex = -1;
        });
    }

    void Renderer::registerGameObject(GameObject* node)
    {
        if (!node) return;
        node->traverse([this](GameObject* go) {
            if (go->isSkinned())
            {
                if (go->skinnedRenderIndex < 0)
                    go->skinnedRenderIndex = addSkinnedMesh(*go->getSkinnedMesh());
            }
            else if (go->hasMesh() && go->staticRenderIndex < 0)
            {
                go->staticRenderIndex = addStaticMesh(*go->getMesh());
            }
        });
    }

    void Renderer::removeMeshComponent(GameObject* go)
    {
        if (!go || !go->hasMesh()) return;
        // No vkDeviceWaitIdle: removeStaticObject/removeSkinnedObject enqueue
        // the destruction kDelayFrames frames out, which is longer than any
        // in-flight command buffer can take.
        if (go->staticRenderIndex >= 0)
            removeStaticObject(go->staticRenderIndex);
        go->staticRenderIndex = -1;
        // Since the import detects rigs, the editor does create skinned meshes:
        // without this, removing the component leaks its render object on the GPU and leaves
        // the stale index, which the rest of the code takes as valid.
        if (go->skinnedRenderIndex >= 0)
            removeSkinnedObject(go->skinnedRenderIndex);
        go->skinnedRenderIndex = -1;
        go->setMesh(nullptr);
    }

    void Renderer::replaceStaticTextureWithMissing(int renderIndex, TextureSlot slot)
    {
        if (renderIndex < 0 || renderIndex >= (int)m_objects.size()) return;
        // The texture lives in the shared entry, so the checkerboard is
        // seen by ALL the objects that share that mesh+material. That is
        // correct: the asset that disappeared is the same for all of them.
        SharedGpuMesh* gpuPtr = m_sharedMeshes.get(m_objects[renderIndex].sharedIndex);
        if (!gpuPtr) return;
        SharedGpuMesh& obj = *gpuPtr;

        // Two different problems, and only one of them was solved by the deferred queue.
        //
        // The RESOURCES were safe: the old handles are enqueued (see further
        // down) instead of being destroyed now, so an in-flight command buffer
        // that still references the descriptor set keeps seeing them valid until
        // the queue releases them kDelayFrames frames later.
        //
        // What was NOT safe is the descriptor set itself: writing it
        // while a command buffer that has it bound is still in flight is
        // invalid use per the specification (UPDATE_AFTER_BIND is needed, which
        // these sets do not request) however well the resources hold up (H25).
        //
        // We wait for the GPU before touching it. It is expensive and it does not matter: this only
        // runs when a texture could not be loaded, that is, once per
        // broken asset and never per frame.

        VkImage*        img     = nullptr;
        VkDeviceMemory* mem     = nullptr;
        VkImageView*    view    = nullptr;
        VkSampler*      sampler = nullptr;
        uint32_t        binding = 1;

        switch (slot)
        {
            case TextureSlot::Diffuse:
                img = &obj.textureImage; mem = &obj.textureMem; view = &obj.textureView; sampler = &obj.sampler;
                binding = 1;
                break;
            case TextureSlot::Normal:
                img = &obj.normalImage; mem = &obj.normalMem; view = &obj.normalView; sampler = &obj.normalSampler;
                binding = 2;
                break;
            case TextureSlot::MetallicRoughness:
                img = &obj.ormImage; mem = &obj.ormMem; view = &obj.ormView; sampler = &obj.ormSampler;
                binding = 4;
                break;
        }

        // The three old handles are still referenced by the descriptor set
        // that an in-flight command buffer may be using. They are enqueued by
        // value: capturing the img/mem/view pointers would read the NEW ones
        // when the lambda ran, three frames later.
        //
        // The sampler does NOT go in: since it is the shared one from
        // GpuResources::sharedMaterialSampler, destroying the old one here would
        // take down the one of ALL the other materials. And the failure would
        // not show on load but only when changing a texture from the editor,
        // which is exactly what no test covers.
        const VkImage        oldImage   = *img;
        const VkDeviceMemory oldMem     = *mem;
        const VkImageView    oldView    = *view;
        // If the old one was a SHARED fill-in, the image and its memory
        // are not this mesh's and destroying them would take down those of all the
        // others. It is asked HERE and not inside the lambda because the lambda
        // only receives the VkDevice; the set of shared ones does not change after
        // being created, so asking now is just as valid.
        const bool prestada = m_res.isSharedPlaceholder(oldImage);
        m_deferredDeletes.push([oldImage, oldMem, oldView, prestada](VkDevice dev) {
            // The view WAS this mesh's in both cases.
            vkDestroyImageView(dev, oldView, nullptr);
            if (prestada) return;
            vkDestroyImage(dev, oldImage, nullptr);
            vkFreeMemory(dev,   oldMem,   nullptr);
        });

        // empty path + no embedded bytes = createTextureImage generates the
        // "missing" fallback checkerboard (same path as a model
        // loaded without a texture). createTextureImage creates the VkImage with
        // format VK_FORMAT_R8G8B8A8_SRGB (hardcoded) for all the slots;
        // that is why the image view is also created in SRGB (default format of
        // createTextureImageView) for the three slots, even though
        // Normal/MetallicRoughness normally use UNORM: the image is not
        // created with VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT, so the view must
        // use the exact same format the image was created with or the
        // validation layer fires VUID-VkImageViewCreateInfo-image-01762.
        m_res.createTextureImage("", {}, *img, *mem);
        m_res.createTextureImageView(*img, *view);
        *sampler = m_res.sharedMaterialSampler();

        // The wait goes here and not above: what has to be protected is the write
        // of the set, not the creation of the new image.
        vkDeviceWaitIdle(m_gpu.device());

        for (int i = 0; i < MAX_FRAMES; i++)
        {
            VkDescriptorImageInfo imageInfo{};
            imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            imageInfo.imageView   = *view;
            imageInfo.sampler     = *sampler;

            VkWriteDescriptorSet write{};
            write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write.dstSet          = obj.descriptorSets[i];
            write.dstBinding      = binding;
            write.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            write.descriptorCount = 1;
            write.pImageInfo      = &imageInfo;

            vkUpdateDescriptorSets(m_gpu.device(), 1, &write, 0, nullptr);
        }
    }

    void Renderer::rebuildStaticMesh(int index, const Mesh& mesh)
    {
        if (index < 0 || index >= (int)m_objects.size()) return;
        RenderObject& obj = m_objects[(size_t)index];

        const int      viejo  = obj.sharedIndex;
        SharedGpuMesh* gpuPtr = m_sharedMeshes.get(viejo);
        // sharedIndex at -1 (object already released from the editor): there is no entry
        // to redo, and creating it here would resurrect an object nobody asked for.
        if (!gpuPtr) return;

        const std::string nuevaClave = makeSharedMeshKey(mesh);

        // GENERAL path: this object splits off to the entry of `nuevaClave`
        // (creating it if it did not exist) and releases the old one. Valid for any
        // change, geometry included, because it uploads everything again.
        //
        // The acquire goes BEFORE the release on purpose: the other way around, releasing the
        // last owner would return its slot to the freelist and the acquire could
        // reuse THAT same slot for the new entry, just when the
        // destruction of the previous one is still enqueued over a copy of its
        // handles (see SharedGpuMeshCache::release). In this order the old
        // slot is not released until the new one has already been taken.
        auto separarAEntradaPropia = [&]() {
            if (!m_pendingBatch)
                m_pendingBatch = std::make_unique<TransferBatch>(m_gpu);

            bool      creada = false;
            const int nuevo  = m_sharedMeshes.acquire(
                nuevaClave,
                [&](SharedGpuMesh& g) { createSharedGpuMesh(mesh, g, m_pendingBatch.get(), nullptr); },
                &creada);

            // The old one through the SAME path as deleting the object: if this was its
            // last owner, the real destruction does NOT happen here but
            // kDelayFrames frames later, because an in-flight command buffer
            // still references it. releaseRenderObject leaves sharedIndex at -1,
            // so the order matters: first release, then point.
            releaseRenderObject(obj);
            obj.sharedIndex = nuevo;

            if (creada)
            {
                // The two things addStaticMesh does with a newly created
                // entry, and both are needed: allocate its descriptor set, and
                // mark it with the ticket of the current batch so that it is not drawn
                // with its textures still in TRANSFER_DST_OPTIMAL.
                SharedGpuMesh& nueva = *m_sharedMeshes.get(nuevo);
                allocateObjectDescriptorSet(nueva);
                nueva.uploadTicket = m_nextUploadTicket;
            }
        };

        // `obj.name` is NOT refreshed here, and it is not an oversight: buildRenderObject
        // does it because it is the REGISTRATION path, where the RenderObject
        // has just been born empty. Here the object already has the name the
        // editor gave it, it is per-instance and debug data (it does not go into the dedup
        // key nor a single byte of what gets uploaded), so overwriting it from the
        // mesh would put a second responsibility nobody asked for into a call
        // that deals with the material.

        // The first two fields of the key are, in the clear and separated by '|',
        // the vertex count and the index count: makeSharedMeshKey puts them
        // first precisely because they are exact discriminants. Comparing that
        // prefix against the old key's covers both counts without
        // rehashing the mesh.
        auto prefijoGeometria = [](const std::string& clave) {
            const size_t primera = clave.find('|');
            if (primera == std::string::npos) return clave;
            // npos = there is no second '|' (a key that did not come out of
            // makeSharedMeshKey): the whole string is compared, which is the conservative
            // side; at most it sends it down the general path.
            return clave.substr(0, clave.find('|', primera + 1));
        };

        // With more than one owner it cannot be mutated in place: the texture of the other
        // objects would change, and they asked for nothing.
        //
        // And with the geometry changed neither, even if the owner is unique: the
        // fast path does not upload vertices or indices again, so the
        // entry would be re-keyed to a key that describes a geometry it
        // does not have, and the next object that asked for that key would get the
        // old one (silent and SHARED corruption, the kind the dedup
        // exists to avoid).
        //
        // Careful with what this prefix proves and what it does not: it compares the two
        // COUNTS, not the content. A mesh with the same counted vertices and
        // indices but moved slips through, and that is why the header's contract
        // still says that changing the geometry through here is not
        // supported. What it closes is the realistic case (a different mesh) without
        // paying a rehash per click.
        if (m_sharedMeshes.refCount(viejo) > 1 ||
            prefijoGeometria(m_sharedMeshes.keyOf(viejo)) != prefijoGeometria(nuevaClave))
        {
            separarAEntradaPropia();
            return;
        }

        // FAST path: unique owner and same geometry. The three images are
        // replaced in place and the descriptors rewritten; the vertex and index
        // buffers, the descriptor set and the entry's index
        // stay as they are, so the object's sharedIndex is still valid and
        // whatever uploadTicket it had still describes its geometry.
        SharedGpuMesh& gpu = *gpuPtr;

        struct SlotRefs
        {
            VkImage*        img;
            VkDeviceMemory* mem;
            VkImageView*    view;
            VkSampler*      sampler;
            uint32_t        binding;
        };
        const SlotRefs slots[3] = {
            { &gpu.textureImage, &gpu.textureMem, &gpu.textureView, &gpu.sampler,       1 },
            { &gpu.normalImage,  &gpu.normalMem,  &gpu.normalView,  &gpu.normalSampler, 2 },
            { &gpu.ormImage,     &gpu.ormMem,     &gpu.ormView,     &gpu.ormSampler,    4 },
        };

        // The three old ones, enqueued BEFORE the new ones overwrite the fields.
        // Same three precautions as replaceStaticTextureWithMissing, and for the
        // same reasons:
        //
        //  - By VALUE, not capturing the pointers: the lambda runs
        //    kDelayFrames frames later and by then *s.img is already the NEW
        //    image, so capturing the pointer would destroy exactly the one that was just
        //    created.
        //  - Enqueued and not destroyed now: the descriptor set that names them
        //    may be bound in an in-flight command buffer.
        //  - `prestada` (borrowed) is resolved HERE and not inside the lambda. The fill-in ones
        //    belong to GpuResources and are shared by all the meshes without a material:
        //    destroying them would take down those of all the others. It is
        //    asked now because the set of shared ones does not change after
        //    being created, and because isSharedPlaceholder decides by COMPARING
        //    handles: in the teardown those handles are nulled and the guard would stop
        //    recognizing them (H79).
        //
        // The sampler does not go in the queue: it is the one shared by all the
        // materials (sharedMaterialSampler) and destroying it would leave the
        // whole scene without a sampler.
        for (const SlotRefs& s : slots)
        {
            const VkImage        oldImage = *s.img;
            const VkDeviceMemory oldMem   = *s.mem;
            const VkImageView    oldView  = *s.view;
            const bool           prestada = m_res.isSharedPlaceholder(oldImage);
            m_deferredDeletes.push([oldImage, oldMem, oldView, prestada](VkDevice dev) {
                // The view WAS this mesh's in both cases: createTextureImageView
                // creates it per entry, also over the borrowed one.
                vkDestroyImageView(dev, oldView, nullptr);
                if (prestada) return;
                vkDestroyImage(dev, oldImage, nullptr);
                vkFreeMemory(dev,   oldMem,   nullptr);
            });
        }

        // The three new ones through the same path and with the same formats as
        // createSharedGpuMesh: the format returned by createTextureImage /
        // createNormalMapImage (resolveSrgb: slot + sidecar). The image is not created
        // with MUTABLE_FORMAT, so the view has to declare EXACTLY the
        // format it was created with.
        //
        // Without TransferBatch: the synchronous variant waits on the queue itself,
        // so on return the images are already readable. It is what this
        // path wants (a user click, not a frame) and it avoids having to touch the
        // uploadTicket of an entry that is already being drawn.
        VkFormat albedoFmt = VK_FORMAT_R8G8B8A8_SRGB;
        m_res.createTextureImage(mesh.material.texturePath, mesh.material.embeddedTexture,
                                 gpu.textureImage, gpu.textureMem, nullptr, &albedoFmt);
        m_res.createTextureImageView(gpu.textureImage, gpu.textureView, albedoFmt);
        gpu.sampler = m_res.sharedMaterialSampler();

        VkFormat normalFmt = VK_FORMAT_R8G8B8A8_UNORM;
        m_res.createNormalMapImage(mesh.material.normalMapPath, mesh.material.embeddedNormalMap,
                                   gpu.normalImage, gpu.normalMem, nullptr, &normalFmt);
        m_res.createTextureImageView(gpu.normalImage, gpu.normalView, normalFmt);
        gpu.normalSampler = m_res.sharedMaterialSampler();

        // Same split as addStaticMesh: with an ORM map the factors are 1 and
        // the texture supplies them; without it, the shared white one and the
        // material's factors. chooseTextureSource is the single place that decides where
        // the pixels come from (the path wins over embedded bytes).
        VkFormat ormFmt = VK_FORMAT_R8G8B8A8_UNORM;
        if (chooseTextureSource(mesh.material.metallicRoughnessPath,
                                mesh.material.embeddedMetallicRoughness) != TextureSource::None)
        {
            m_res.createNormalMapImage(mesh.material.metallicRoughnessPath,
                                       mesh.material.embeddedMetallicRoughness,
                                       gpu.ormImage, gpu.ormMem, nullptr, &ormFmt);
            gpu.hasOrmMap = true;
        }
        else
        {
            m_res.sharedWhiteOrm(gpu.ormImage, gpu.ormMem);
            gpu.hasOrmMap = false;
        }
        m_res.createTextureImageView(gpu.ormImage, gpu.ormView, ormFmt);
        gpu.ormSampler = m_res.sharedMaterialSampler();

        // The wait goes HERE and not before creating the images: what has to be
        // protected is the WRITE of the set. Writing one that an in-flight command buffer
        // has bound is invalid use per the specification (UPDATE_AFTER_BIND
        // would be needed, which these sets do not request) however well the
        // resources hold up (H25). It is expensive and it does not matter: this runs when the
        // user changes a texture, not per frame.
        vkDeviceWaitIdle(m_gpu.device());

        for (int i = 0; i < MAX_FRAMES; i++)
            for (const SlotRefs& s : slots)
            {
                VkDescriptorImageInfo imageInfo{};
                imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                imageInfo.imageView   = *s.view;
                imageInfo.sampler     = *s.sampler;

                VkWriteDescriptorSet write{};
                write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                write.dstSet          = gpu.descriptorSets[i];
                write.dstBinding      = s.binding;
                write.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                write.descriptorCount = 1;
                write.pImageInfo      = &imageInfo;
                vkUpdateDescriptorSets(m_gpu.device(), 1, &write, 0, nullptr);
            }

        // Last: the entry's content no longer corresponds to its key, and
        // without re-keying the next object that asked for the old one would receive this
        // mesh with the new texture.
        //
        // And it goes LAST on purpose, even if that means throwing away three image
        // uploads and a vkDeviceWaitIdle when the rekey is rejected. Moving it earlier
        // would be cheaper in that case and worse in all the others: between the rekey and here there are
        // four calls that can throw (createTextureImage throws
        // runtime_error if it cannot create the image or the staging buffer), and
        // with the entry already announced under the new key, leaving by exception
        // halfway would leave it PUBLISHED with a half-made material: the
        // next object that asked for that key would acquire it and share the
        // wreckage. By re-keying at the end, an exception halfway leaves an
        // incoherent entry but still under its old key and with a single
        // owner, which is this object, so the damage does not leave here. Cheap on the
        // rare path, contained in the worst.
        //
        // The rekey is rejected when ANOTHER entry already has this exact key
        // (two with the same one would leave one unreachable in the map, that is, a
        // leak of GPU resources nobody would release). Then this object moves to that
        // other one and releases its own, which nobody wants anymore.
        //
        // General rule of the lambda: after its acquire, `gpu` and `gpuPtr` cannot
        // be touched, because an acquire that CREATES an entry can make the cache's
        // vector grow and leave them dangling. Here in particular it does not grow
        // (if the rekey was rejected it is because that key already has a live entry,
        // so the acquire finds it and creates nothing), but the code does not
        // rely on that: the rule belongs to the lambda, which also runs from the
        // general path, where it can create.
        if (!m_sharedMeshes.rekey(viejo, nuevaClave))
            separarAEntradaPropia();
    }

    // ─── Offscreen images ────────────────────────────────────────────────────────

    void Renderer::createOffscreenImages()
    {
        // Sampler shared between the two offscreen frames
        VkSamplerCreateInfo samplerInfo{};
        samplerInfo.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        samplerInfo.magFilter    = VK_FILTER_LINEAR;
        samplerInfo.minFilter    = VK_FILTER_LINEAR;
        samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.borderColor  = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
        if (vkCreateSampler(m_gpu.device(), &samplerInfo, nullptr, &m_offscreenSampler) != VK_SUCCESS)
            throw std::runtime_error("failed to create offscreen sampler!");

        for (int i = 0; i < MAX_FRAMES; i++)
        {
            // Offscreen color image (LDR, already tonemapped). It is written by the
            // composition pass; the scene goes to m_hdrImage.
            m_res.createImage(
                effectiveViewport().width, effectiveViewport().height,
                m_swapChainFormat,
                VK_IMAGE_TILING_OPTIMAL,
                // TRANSFER_SRC: source of the blit to the swapchain in headless.
                VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                    VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                m_offscreenImage[i], m_offscreenMemory[i]);

            m_res.createTextureImageView(m_offscreenImage[i], m_offscreenView[i], m_swapChainFormat);

            // UI pass framebuffer: the SAME final image, at the output size.
            // It is created here because it dies and is reborn with it.
            VkFramebufferCreateInfo uiFb{};
            uiFb.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            uiFb.renderPass      = m_uiRenderPass;
            uiFb.attachmentCount = 1;
            uiFb.pAttachments    = &m_offscreenView[i];
            uiFb.width           = effectiveViewport().width;
            uiFb.height          = effectiveViewport().height;
            uiFb.layers          = 1;
            if (vkCreateFramebuffer(m_gpu.device(), &uiFb, nullptr, &m_uiFramebuffer[i]) != VK_SUCCESS)
                throw std::runtime_error("failed to create ui framebuffer!");

            // Scene HDR image: INTERNAL size (which with SSAA is larger than
            // the window's), float format. It is read by the bloom downsample
            // (compute) and the composition (fragment). With MSAA it is the resolve
            // destination, not the rasterizer's direct target.
            m_res.createImage(
                m_renderExtent.width, m_renderExtent.height,
                kHdrFormat,
                VK_IMAGE_TILING_OPTIMAL,
                // STORAGE in addition to SAMPLED: ssr_resolve.comp adds the reflection
                // over this same image (imageLoad + imageStore of the SAME
                // texel) before the bloom reads it. R16G16B16A16_SFLOAT is one of
                // the mandatory formats as storage image.
                // TRANSFER_SRC: source of the blit to the cubemap face when
                // a reflection probe is baked. It changes nothing in normal rendering.
                // TRANSFER_DST: destination of the motion blur's copy back,
                // which blurs into a separate image because it reads arbitrary
                // pixels. When off, no copy is recorded.
                VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                    VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                    VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                m_hdrImage[i], m_hdrMemory[i]);

            m_res.createTextureImageView(m_hdrImage[i], m_hdrView[i], kHdrFormat);

            // Register the texture in the UI layer to obtain the
            // VkDescriptorSet. In headless nobody samples it: the descriptor
            // set stays null and destroyOffscreenImages already checks before
            // freeing it.
            if (!m_headless && m_ui)
                m_offscreenDescSet[i] = m_ui->registerUiTexture(
                    reinterpret_cast<uint64_t>(m_offscreenSampler),
                    reinterpret_cast<uint64_t>(m_offscreenView[i]));
        }

        // BEFORE createAaImages: the TAA descriptor set references
        // the pre-pass depth. Its three images go at the
        // internal render size, like the rest of the intermediate targets.
        m_depthPrepass.createImages(depthPrepassCtx());
        m_ssaoPass.createImages(ssaoCtx());
        // The per-object sets hold the old view: they have to be revisited. At
        // startup there are none yet and the loop does nothing.
        refreshSsaoDescriptors();
        // AFTER createSsaoImages: the culling descriptor set references
        // the depth, which was just created there. The grid is sized with
        // m_renderExtent, like the rest of the intermediate targets.
        m_fpPass.createBuffers(fpCtx());
        // BEFORE the scene and composition framebuffers: with MSAA their color
        // attachments are the multisample images that are created there.
        createMsaaImages();
        m_aaPass.createImages(aaCtx());

        const bool msaa = (m_aaSampleCount != VK_SAMPLE_COUNT_1_BIT);

        for (int i = 0; i < MAX_FRAMES; i++)
        {
            // Scene framebuffer: HDR + shared depth. With MSAA it
            // rasterizes over the multisample color and the usual HDR becomes
            // the resolve destination, in the third slot.
            VkImageView atts[3] = { msaa ? m_msaaHdrView[i] : m_hdrView[i], m_depthImageView, m_hdrView[i] };
            VkFramebufferCreateInfo fbInfo{};
            fbInfo.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            fbInfo.renderPass      = m_offscreenRenderPass;
            fbInfo.attachmentCount = msaa ? 3 : 2;
            fbInfo.pAttachments    = atts;
            fbInfo.width           = m_renderExtent.width;
            fbInfo.height          = m_renderExtent.height;
            fbInfo.layers          = 1;
            if (vkCreateFramebuffer(m_gpu.device(), &fbInfo, nullptr, &m_offscreenFramebuffer[i]) != VK_SUCCESS)
                throw std::runtime_error("failed to create offscreen framebuffer!");

            // Composition framebuffer: LDR + the SAME depth (loaded). Only
            // needed when the composition writes DIRECTLY into the
            // offscreen; if the active mode puts a resolve pass behind it, the
            // destination is the intermediate image (m_aaSrcFramebuffer) and this one would not
            // be used. Also with SSAA it would be invalid: the depth has the internal
            // size and the offscreen the window's.
            if (needsAaIntermediate()) continue;

            VkImageView compAtts[3] = { msaa ? m_msaaLdrView[i] : m_offscreenView[i], m_depthImageView, m_offscreenView[i] };
            VkFramebufferCreateInfo compFb = fbInfo;
            compFb.renderPass      = m_compositeRenderPass;
            compFb.attachmentCount = msaa ? 3 : 2;
            compFb.pAttachments    = compAtts;
            if (vkCreateFramebuffer(m_gpu.device(), &compFb, nullptr, &m_compositeFramebuffer[i]) != VK_SUCCESS)
                throw std::runtime_error("failed to create composite framebuffer!");
        }

        // Depends on m_hdrView (the sets of the first downsample and of the
        // composition reference it), so it goes after the loop.
        createBloomImages();
        // Depends on m_hdrView (the sum's set references it) and on
        // the pre-pass depth (the march's), so it goes after both.
        m_ssrPass.createImages(ssrCtx());
        // Depends on m_hdrView and on the pre-pass depth just like the SSR. On the
        // first init it exits through the guard (the UBO does not exist yet) and the
        // end of init redoes it.
        m_fogPass.createSets(fogCtx());
        // Depends on m_hdrView and on the pre-pass depth just like the SSR.
        m_motionBlurPass.createImages(motionBlurCtx());
        printf("offscreen images OK\n"); fflush(stdout);
    }

    void Renderer::destroyOffscreenImages()
    {
        destroyBloomImages();
        m_ssaoPass.destroyImages(ssaoCtx());
        m_depthPrepass.destroyImages(depthPrepassCtx());
        m_fpPass.destroyBuffers(fpCtx());
        m_ssrPass.destroyImages(ssrCtx());
        m_fogPass.destroySets();
        m_motionBlurPass.destroyImages(motionBlurCtx());
        m_aaPass.destroyImages(aaCtx());
        destroyMsaaImages();
        for (int i = 0; i < MAX_FRAMES; i++)
        {
            if (m_offscreenDescSet[i] && m_ui)
            {
                m_ui->unregisterUiTexture(m_offscreenDescSet[i]);
                m_offscreenDescSet[i] = 0;
            }
            if (m_compositeFramebuffer[i])
            {
                vkDestroyFramebuffer(m_gpu.device(), m_compositeFramebuffer[i], nullptr);
                m_compositeFramebuffer[i] = VK_NULL_HANDLE;
            }
            if (m_hdrView[i])
            {
                vkDestroyImageView(m_gpu.device(), m_hdrView[i], nullptr);
                m_hdrView[i] = VK_NULL_HANDLE;
            }
            if (m_hdrImage[i])
            {
                vkDestroyImage(m_gpu.device(), m_hdrImage[i], nullptr);
                m_hdrImage[i] = VK_NULL_HANDLE;
            }
            if (m_hdrMemory[i])
            {
                vkFreeMemory(m_gpu.device(), m_hdrMemory[i], nullptr);
                m_hdrMemory[i] = VK_NULL_HANDLE;
            }
            if (m_offscreenFramebuffer[i])
            {
                vkDestroyFramebuffer(m_gpu.device(), m_offscreenFramebuffer[i], nullptr);
                m_offscreenFramebuffer[i] = VK_NULL_HANDLE;
            }
            // Before the view that references it.
            if (m_uiFramebuffer[i])
            {
                vkDestroyFramebuffer(m_gpu.device(), m_uiFramebuffer[i], nullptr);
                m_uiFramebuffer[i] = VK_NULL_HANDLE;
            }
            if (m_offscreenView[i])
            {
                vkDestroyImageView(m_gpu.device(), m_offscreenView[i], nullptr);
                m_offscreenView[i] = VK_NULL_HANDLE;
            }
            if (m_offscreenImage[i])
            {
                vkDestroyImage(m_gpu.device(), m_offscreenImage[i], nullptr);
                m_offscreenImage[i] = VK_NULL_HANDLE;
            }
            if (m_offscreenMemory[i])
            {
                vkFreeMemory(m_gpu.device(), m_offscreenMemory[i], nullptr);
                m_offscreenMemory[i] = VK_NULL_HANDLE;
            }
        }
        if (m_offscreenSampler)
        {
            vkDestroySampler(m_gpu.device(), m_offscreenSampler, nullptr);
            m_offscreenSampler = VK_NULL_HANDLE;
        }
    }

    // The bloom pass lives in BloomPass; what remains here is the COMPOSITION,
    // which is not its: it adds mip 0 over the HDR, tonemaps and also hosts the
    // selection outline, the gizmos and the game UI.
    void Renderer::createBloomPipelines()
    {
        // --- GPU cost measurement -------------------------------------------
        // Device properties shared by all the passes; they are resolved
        // here because the bloom is the first to ask for a query pool.
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(m_gpu.physicalDevice(), &props);
        m_timestampPeriod     = props.limits.timestampPeriod;
        m_timestampsSupported = props.limits.timestampComputeAndGraphics && m_timestampPeriod > 0.0f;

        m_bloomPass.createPipelines(bloomCtx());

        // The composition layout used to reuse the bloom's VkDescriptorSetLayoutCreateInfo:
        // here it has to be declared, with the same two bindings.
        VkDescriptorSetLayoutCreateInfo dsl{};
        dsl.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        dsl.bindingCount = 2;

        // --- Composition: HDR scene + bloom mip 0 -----------------------
        VkDescriptorSetLayoutBinding compBindings[2]{};
        for (int i = 0; i < 2; i++)
        {
            compBindings[i].binding         = (uint32_t)i;
            compBindings[i].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            compBindings[i].descriptorCount = 1;
            compBindings[i].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
        }
        dsl.pBindings = compBindings;
        if (vkCreateDescriptorSetLayout(m_gpu.device(), &dsl, nullptr, &m_compositeDescLayout) != VK_SUCCESS)
            throw std::runtime_error("failed to create composite descriptor set layout!");

        VkDescriptorPoolSize compSize{};
        compSize.type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        compSize.descriptorCount = MAX_FRAMES * 2;

        VkDescriptorPoolCreateInfo compDpi{};
        compDpi.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        compDpi.poolSizeCount = 1;
        compDpi.pPoolSizes    = &compSize;
        compDpi.maxSets       = MAX_FRAMES;
        if (vkCreateDescriptorPool(m_gpu.device(), &compDpi, nullptr, &m_compositeDescPool) != VK_SUCCESS)
            throw std::runtime_error("failed to create composite descriptor pool!");

        VkPushConstantRange compPcr{};
        compPcr.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        compPcr.offset     = 0;
        compPcr.size       = sizeof(float);   // intensity

        VkPipelineLayoutCreateInfo compPli{};
        compPli.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        compPli.setLayoutCount         = 1;
        compPli.pSetLayouts            = &m_compositeDescLayout;
        compPli.pushConstantRangeCount = 1;
        compPli.pPushConstantRanges    = &compPcr;
        if (vkCreatePipelineLayout(m_gpu.device(), &compPli, nullptr, &m_compositePipelineLayout) != VK_SUCCESS)
            throw std::runtime_error("failed to create composite pipeline layout!");

        recreateCompositePipeline();

        printf("bloom pipelines OK\n"); fflush(stdout);
    }

    void Renderer::recreateCompositePipeline()
    {
        auto compVertCode = loadShaderFile("shaders/fullscreen.vert.spv");
        auto compFragCode = loadShaderFile("shaders/bloom_composite.frag.spv");
        VkShaderModule compVertModule = createShaderModule(compVertCode);
        VkShaderModule compFragModule = createShaderModule(compFragCode);

        VkPipelineShaderStageCreateInfo compStages[2]{};
        compStages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        compStages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
        compStages[0].module = compVertModule;
        compStages[0].pName  = "main";
        compStages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        compStages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
        compStages[1].module = compFragModule;
        compStages[1].pName  = "main";

        // No vertex buffer: the three vertices come from gl_VertexIndex.
        VkPipelineVertexInputStateCreateInfo compVi{};
        compVi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

        VkPipelineInputAssemblyStateCreateInfo compIa{};
        compIa.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        compIa.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

        VkPipelineViewportStateCreateInfo compVp{};
        compVp.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        compVp.viewportCount = 1;
        compVp.scissorCount  = 1;

        VkPipelineRasterizationStateCreateInfo compRs{};
        compRs.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        compRs.polygonMode = VK_POLYGON_MODE_FILL;
        // NONE: the triangle is generated in the shader and its orientation does not depend
        // on any frontFace of the rest of the engine.
        compRs.cullMode    = VK_CULL_MODE_NONE;
        compRs.lineWidth   = 1.0f;

        VkPipelineMultisampleStateCreateInfo compMs{};
        compMs.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        // The composition triangle also goes to the multisample pass when there is
        // MSAA: what gets resolved at the end is the whole pass, and this
        // pipeline has to declare the same samples as its companions.
        compMs.rasterizationSamples = m_aaSampleCount;

        // No depth test or write: the triangle covers the screen and
        // the loaded depth has to reach the outline and the gizmos INTACT,
        // which are drawn right after in this same pass.
        VkPipelineDepthStencilStateCreateInfo compDs{};
        compDs.sType            = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        compDs.depthTestEnable  = VK_FALSE;
        compDs.depthWriteEnable = VK_FALSE;
        compDs.depthCompareOp   = VK_COMPARE_OP_ALWAYS;

        VkPipelineColorBlendAttachmentState compBlend{};
        compBlend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                   VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

        VkPipelineColorBlendStateCreateInfo compCb{};
        compCb.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        compCb.attachmentCount = 1;
        compCb.pAttachments    = &compBlend;

        VkDynamicState compDynStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo compDyn{};
        compDyn.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        compDyn.dynamicStateCount = 2;
        compDyn.pDynamicStates    = compDynStates;

        VkGraphicsPipelineCreateInfo compPci{};
        compPci.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        compPci.stageCount          = 2;
        compPci.pStages             = compStages;
        compPci.pVertexInputState   = &compVi;
        compPci.pInputAssemblyState = &compIa;
        compPci.pViewportState      = &compVp;
        compPci.pRasterizationState = &compRs;
        compPci.pMultisampleState   = &compMs;
        compPci.pDepthStencilState  = &compDs;
        compPci.pColorBlendState    = &compCb;
        compPci.pDynamicState       = &compDyn;
        compPci.layout              = m_compositePipelineLayout;
        compPci.renderPass          = m_compositeRenderPass;
        compPci.subpass             = 0;

        if (vkCreateGraphicsPipelines(m_gpu.device(), VK_NULL_HANDLE, 1, &compPci, nullptr, &m_compositePipeline) != VK_SUCCESS)
            throw std::runtime_error("failed to create composite pipeline!");

        vkDestroyShaderModule(m_gpu.device(), compVertModule, nullptr);
        vkDestroyShaderModule(m_gpu.device(), compFragModule, nullptr);
    }

    void Renderer::createBloomImages()
    {
        m_bloomPass.createImages(bloomCtx());
        // The sets are ALWAYS created, whether or not there is a chain. Before it returned here
        // when the viewport was tiny (<4 px, which is when mipCount ends up
        // at 0) and m_compositeSets stayed at VK_NULL_HANDLE... which is
        // exactly what recordCommandBuffer binds without asking. And the set
        // is not only the bloom's: its binding 0 is the HDR scene, without which the
        // composition (which is what tonemaps) has no input at all.
        createCompositeSets();
    }

    // The two bindings bloom_composite.frag reads: the HDR scene and mip 0 of
    // the bloom chain. The sampler is the bloom's, which serves for both.
    void Renderer::createCompositeSets()
    {
        vkResetDescriptorPool(m_gpu.device(), m_compositeDescPool, 0);

        for (int f = 0; f < MAX_FRAMES; f++)
        {
            VkDescriptorSetAllocateInfo cai{};
            cai.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            cai.descriptorPool     = m_compositeDescPool;
            cai.descriptorSetCount = 1;
            cai.pSetLayouts        = &m_compositeDescLayout;
            if (vkAllocateDescriptorSets(m_gpu.device(), &cai, &m_compositeSets[f]) != VK_SUCCESS)
                throw std::runtime_error("failed to allocate composite descriptor set!");

            VkDescriptorImageInfo compInfos[2]{};
            compInfos[0].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            compInfos[0].imageView   = m_hdrView[f];
            compInfos[0].sampler     = m_bloomPass.sampler();
            // Without a chain there is no mip 0 to point at, but the descriptor has
            // to be valid all the same. The scene is repeated in the bloom
            // slot: recordCommandBuffer already forces the intensity to 0 when
            // mipCount is 0, so it adds nothing. Careful with the layout, which is NOT the
            // same: the mip lives in GENERAL and the scene in SHADER_READ_ONLY.
            //
            // The scene is repeated and a black image is not invented because here
            // the substitute is a RENDERED image, not unwritten memory:
            // the dangerous case of the D3D12 backend (inf * 0 = NaN over a heap
            // not zeroed) does not apply.
            const bool haveChain     = m_bloomPass.mipCount() > 0;
            compInfos[1].imageLayout = haveChain ? VK_IMAGE_LAYOUT_GENERAL
                                                 : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            compInfos[1].imageView   = haveChain ? m_bloomPass.mipView0(f) : m_hdrView[f];
            compInfos[1].sampler     = m_bloomPass.sampler();

            VkWriteDescriptorSet compWrites[2]{};
            for (int b = 0; b < 2; b++)
            {
                compWrites[b].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                compWrites[b].dstSet          = m_compositeSets[f];
                compWrites[b].dstBinding      = (uint32_t)b;
                compWrites[b].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                compWrites[b].descriptorCount = 1;
                compWrites[b].pImageInfo      = &compInfos[b];
            }
            vkUpdateDescriptorSets(m_gpu.device(), 2, compWrites, 0, nullptr);
        }
    }

    void Renderer::destroyBloomImages()
    {
        m_bloomPass.destroyImages(bloomCtx());
        // The composition sets die with the pool reset that
        // createCompositeSets does; here only the handles are nulled.
        for (int f = 0; f < MAX_FRAMES; f++) m_compositeSets[f] = VK_NULL_HANDLE;
    }

    void Renderer::setBloomEnabled(bool v)
    {
        if (v == bloomEnabled()) return;
        setBloomEnabledFlag(v);
        // When turned off, the chain keeps the bloom of the last computed frame and
        // the composition keeps sampling it (the shader always multiplies, and
        // 0 * inf would be NaN). A clear to black per in-flight frame leaves it neutral;
        // from then on, zero work.
        if (!v)
            m_bloomPass.markClearPending();
    }

    BloomPass::Context Renderer::bloomCtx()
    {
        return BloomPass::Context{
            m_gpu, *this, m_renderExtent, m_swapChainExtent, m_currentFrame,
            kHdrFormat, m_hdrView, m_timestampsSupported, m_timestampPeriod
        };
    }

    // ── Compute skinning ────────────────────────────────────────────────────
    SkinningPass::Context Renderer::skinningCtx()
    {
        // m_skinnedVisible is the SAME list the draw loop consumes: if
        // the pass skipped an object that is later drawn, it would be left with the pose of the
        // last frame in which it was visible.
        return SkinningPass::Context{ m_gpu, m_skinnedObjects, m_skinnedVisible, (uint32_t)m_currentFrame };
    }

    // ── Shadow map ──────────────────────────────────────────────────────────
    ShadowPass::Context Renderer::shadowCtx()
    {
        // The two sets shadow.vert declares. The pipeline layout that comes out of
        // them is lent by the pass to the depth pre-pass, which declares the same ones.
        return ShadowPass::Context{ m_gpu, m_descriptorSetLayout, m_instanceBuffers.descLayout() };
    }

    // ── Global IBL and probes ───────────────────────────────────────────────
    SelectionOutlinePass::Context Renderer::outlineCtx()
    {
        return SelectionOutlinePass::Context{ m_gpu, m_pipelineLayout, m_compositeRenderPass };
    }

    IblPass::Context Renderer::iblCtx()
    {
        // Without a skybox loaded both views are null and precompute() exits:
        // the cubemaps stay with the neutral ambient.
        return IblPass::Context{
            m_gpu,
            m_skybox.isInitialized() ? m_skybox.cubeView()    : VK_NULL_HANDLE,
            m_skybox.isInitialized() ? m_skybox.cubeSampler() : VK_NULL_HANDLE
        };
    }

    // The only long Context in the engine, and for a reason: the bake REDRAWS the
    // scene. Everything here belongs to the frame's offscreen pass (slot 0) and to
    // the object lists, which stay in the Renderer.
    ReflectionProbePass::Context Renderer::probeCtx()
    {
        return ReflectionProbePass::Context{
            m_gpu, m_scene, m_skybox,
            m_iblPass.irradiancePipeline(), m_iblPass.prefilterPipeline(),
            m_iblPass.pipelineLayout(),     m_iblPass.descPool(),
            m_iblPass.descLayout(),         m_iblPass.sampler(),
            m_iblPass.irradianceView(),     m_iblPass.prefilterView(),
            m_renderExtent, m_offscreenRenderPass, m_offscreenFramebuffer[0],
            m_hdrImage[0], m_pipeline, m_skinnedGfxPipeline, m_pipelineLayout,
            m_instanceBuffers.set(0), m_uniformBuffersMapped[0], m_uboWritten[0],
            m_fpPass,
            m_objects, m_sharedMeshes, m_skinnedObjects, m_lastCompletedTicket,
            m_ssaoPass.blurImage(0),
            m_timestampsSupported, m_timestampPeriod
        };
    }

    // ── SSAO + depth pre-pass ───────────────────────────────────────────────
    DepthPrepassPass::Context Renderer::depthPrepassCtx()
    {
        return DepthPrepassPass::Context{
            m_gpu, m_res, m_renderExtent, m_currentFrame, m_shadowPass.pipelineLayout()
        };
    }

    SsaoPass::Context Renderer::ssaoCtx()
    {
        return SsaoPass::Context{
            m_gpu, m_res, *this, m_renderExtent, m_swapChainExtent, m_currentFrame,
            m_depthPrepass.views(), m_depthPrepass.sampler(),
            m_timestampsSupported, m_timestampPeriod
        };
    }

    void Renderer::recordSsaoPass(VkCommandBuffer cmd, const Frustum& camFrustum, const glm::mat4& proj)
    {
        // Degenerate viewport or resources not yet created: nothing to do.
        if (!m_ssaoPass.ready(m_currentFrame)) return;

        // The SSR feeds from the SAME depth pre-pass: with SSAO off but SSR
        // active it has to be recorded all the same. The only thing decoupled is this; the
        // two occlusion dispatches are still tied to ssaoEnabled().
        // The TAA is the third client: it reprojects the previous frame from
        // this same depth, and it wants it WITHOUT the subpixel jitter, which is
        // exactly how this pre-pass records it (it uses fc.proj, not the jittered one).
        // The fourth client is the tiled Forward+: it reduces each tile's depth
        // maximum from this same image. The clustered one does NOT need it
        // (its grid is analytic) and that is why it does not come in here.
        // The fifth client is the volumetric fog: it unprojects this same
        // depth to know how far to march each pixel. Without this, with
        // the fog on and everything else off, the depth image
        // would not be recorded in the frame.
        // The sixth is the motion blur: it reprojects this same depth to the previous
        // frame to get each pixel's velocity. Without this, turned on and
        // with everything else off, it would read an image nobody wrote.
        const bool ssrNeedsDepth = m_ssrPass.active(ssrCtx()) || m_aaActiveMode == AaMode::Taa ||
                                   m_fpActiveMode == FpMode::Tiled || m_fogEnabled ||
                                   m_motionBlurPass.active(motionBlurCtx());
        m_ssrStampedPrepass = false;

        // Off: leaves the map at identity if there is something to clear.
        // On: reads the slot's timestamps and opens this frame's pair.
        m_ssaoPass.recordPreDepth(ssaoCtx(), cmd);

        // With SSAO off and nobody else asking for the depth, the frame
        // ends here for this pass: zero work recorded.
        if (!ssaoEnabled() && !ssrNeedsDepth) return;

        // SSR queries: all FOUR are reset here, which is the first of its own
        // things recorded in the frame, and the [0,1] pair bounds the pre-pass. With SSAO
        // on, that cost is already measured by its own pair and recordSsrPass does not
        // add it again; the pair is written all the same so that reading all
        // four never gives NOT_READY.
        if (ssrNeedsDepth && m_timestampsSupported)
        {
            vkCmdResetQueryPool(cmd, m_ssrPass.queryPool(), m_currentFrame * 4, 4);
            vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, m_ssrPass.queryPool(), m_currentFrame * 4);
            m_ssrStampedPrepass = true;
        }

        // ── Depth pre-pass: the whole scene, depth only ──────────────────────
        // The target and the pipeline belong to DepthPrepassPass; the draws stay
        // here, which is where the object lists and the instance SSBO are.
        {
            m_depthPrepass.begin(depthPrepassCtx(), cmd);

            // Same guards and same frustum as the scene pass: if something entered
            // here that is not drawn there, the AO would darken against
            // invisible geometry.
            //
            // Characters DO go in, after the static ones. Before they did not, with the
            // argument that the shadow pass did not include them either, and that was
            // false: it does draw them (see bindSkinnedPipeline further down). This
            // depth is not just the AO's: the FOG marches up to it, so
            // without the characters the fog of their silhouette was computed up to whatever
            // was behind and a cutout shaped like the object behind came out.
            // Own stretch of the SSBO, after the cascades' and before the
            // scene pass's.
            gatherAndBatch(camFrustum, /*colorPass*/ false);

            const VkDescriptorSet instSet = m_instanceBuffers.set(m_currentFrame);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_shadowPass.pipelineLayout(),
                1, 1, &instSet, 0, nullptr);

            for (const InstanceBatch& batch : m_instanceBatches)
            {
                const SharedGpuMesh* gpu = m_sharedMeshes.get(batch.sharedIndex);
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_shadowPass.pipelineLayout(),
                    0, 1, &gpu->descriptorSets[m_currentFrame], 0, nullptr);

                VkBuffer vb[] = { gpu->vertexBuffer };
                VkDeviceSize offsets[] = { 0 };
                vkCmdBindVertexBuffers(cmd, 0, 1, vb, offsets);
                vkCmdBindIndexBuffer(cmd, gpu->indexBuffer, 0, VK_INDEX_TYPE_UINT32);
                vkCmdDrawIndexed(cmd, gpu->indexCount, batch.instanceCount, 0, 0, batch.firstInstance);
            }

            // And the characters, with their own pipeline: what is drawn is the
            // skinning compute's OUTPUT, which has another stride. Same
            // guards as in the shadow pass: if the compute did not dispatch for
            // this object, its buffer holds an old pose.
            {
                bool skinnedBound = false;
                for (size_t si = 0; si < m_skinnedObjects.size(); si++)
                {
                    // The SAME visible list the compute consumed, just like
                    // in the shadow pass: an undispatched object would be left with
                    // the pose of the last frame in which it was visible.
                    if (si >= m_skinnedVisible.size() || !m_skinnedVisible[si]) continue;
                    const SkinnedRenderObject& sobj = m_skinnedObjects[si];
                    if (!sobj.meshVisible) continue;
                    if (sobj.outputVertexBuffer == VK_NULL_HANDLE || sobj.matGfx.empty())
                        continue;
                    // No room in this frame's SSBO: better no depth than
                    // overwriting another pass's stretch. Counted, as in the shadow
                    // pass: without a counter there was no way to know (H23).
                    //
                    // The shader takes the model from the SSBO via gl_InstanceIndex: one
                    // entry per object and a one-instance draw with
                    // firstInstance pointing at it. With no room there is no pointer.
                    uint32_t instanceIndex = 0;
                    glm::mat4* slot = m_instanceBuffers.cur().alloc(1, &instanceIndex);
                    if (!slot) { ++m_statInstanceOverflow; break; }
                    *slot = sobj.transform;

                    if (!skinnedBound)
                    {
                        m_depthPrepass.bindSkinnedPipeline(cmd);
                        skinnedBound = true;
                    }

                    // Set 0 only for the UBO: this shader does not sample anything, so
                    // any set of the layout the pipeline declares works.
                    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                        m_shadowPass.pipelineLayout(), 0, 1,
                        &sobj.matGfx[0].descSets[m_currentFrame], 0, nullptr);

                    VkBuffer     svb[]      = { sobj.outputVertexBuffer };
                    VkDeviceSize soffsets[] = { 0 };
                    vkCmdBindVertexBuffers(cmd, 0, 1, svb, soffsets);
                    vkCmdBindIndexBuffer(cmd, sobj.indexBuffer, 0, VK_INDEX_TYPE_UINT32);
                    for (const auto& sm : sobj.subMeshes)
                        vkCmdDrawIndexed(cmd, sm.indexCount, 1, sm.indexStart, 0,
                                         instanceIndex);
                }
            }

            m_depthPrepass.end(cmd);
        }

        if (m_ssrStampedPrepass)
            vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, m_ssrPass.queryPool(), m_currentFrame * 4 + 1);

        // The SSR only wanted the depth: without SSAO there is no occlusion to
        // compute nor map to write.
        if (!ssaoEnabled()) return;

        m_ssaoPass.record(ssaoCtx(), cmd, proj);
    }

    // ── Forward+ ────────────────────────────────────────────────────────────
    ForwardPlusPass::Context Renderer::fpCtx()
    {
        return ForwardPlusPass::Context{
            m_gpu, m_res, m_renderExtent, m_currentFrame, m_fpActiveMode,
            m_depthPrepass.views(), m_depthPrepass.sampler(),
            m_timestampsSupported, m_timestampPeriod
        };
    }

    // ── SSR ─────────────────────────────────────────────────────────────────
    bool Renderer::anyObjectWithSsr() const
    {
        // The traversal stays here: m_objects and m_skinnedObjects belong to the
        // Renderer, not to the pass. SsrPass::active() receives the result.
        for (const RenderObject& o : m_objects)
            if (o.ssrStrength > 0.0f) return true;
        for (const SkinnedRenderObject& o : m_skinnedObjects)
            if (o.ssrStrength > 0.0f) return true;
        return false;
    }

    SsrPass::Context Renderer::ssrCtx()
    {
        return SsrPass::Context{
            m_gpu, m_res, *this, m_renderExtent, m_swapChainExtent, m_currentFrame,
            kHdrFormat, m_hdrImage, m_hdrView, m_depthPrepass.views(), m_depthPrepass.sampler(),
            m_timestampsSupported, m_timestampPeriod,
            anyObjectWithSsr(), m_ssrStampedPrepass
        };
    }

    // ── Volumetric fog ──────────────────────────────────────────────────────
    FogPass::Context Renderer::fogCtx()
    {
        return FogPass::Context{
            m_gpu, *this, m_renderExtent, m_currentFrame,
            m_hdrImage, m_hdrView, m_depthPrepass.views(), m_depthPrepass.sampler(),
            m_uniformBuffers, m_shadowPass.view(), m_shadowPass.sampler(), m_lights,
            m_sceneCenter, m_timestampsSupported, m_timestampPeriod
        };
    }

    // ── Motion blur ─────────────────────────────────────────────────────────
    MotionBlurPass::Context Renderer::motionBlurCtx()
    {
        return MotionBlurPass::Context{
            m_gpu, m_res, *this, m_renderExtent, m_currentFrame, kHdrFormat,
            m_hdrImage, m_hdrView, m_depthPrepass.views(),
            m_ssrPass.sampler(), m_depthPrepass.sampler(),
            m_aaPass.currViewProj(), m_aaPass.prevViewProj(),
            m_timestampsSupported, m_timestampPeriod
        };
    }

    AaPass::Context Renderer::aaCtx()
    {
        return AaPass::Context{
            m_gpu, m_res, *this, m_renderExtent, effectiveViewport(), m_currentFrame,
            m_aaActiveMode, m_swapChainFormat, m_offscreenView, m_depthImageView,
            m_compositeRenderPass, m_depthPrepass.views(), m_depthPrepass.sampler(),
            m_aaQueryPool, m_timestampsSupported, ssaaFactor()
        };
    }

    // The two timestamp pools created with the AA. The AA's stays
    // here because it also times the whole frame without UI ([2,3]), and the
    // Performance panel's because this is the last place in startup where
    // m_timestampsSupported is already resolved and the device is still alive.
    void Renderer::createAaQueryPools()
    {
        if (!m_timestampsSupported) return;

        VkQueryPoolCreateInfo qpi{};
        qpi.sType      = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        qpi.queryType  = VK_QUERY_TYPE_TIMESTAMP;
        qpi.queryCount = MAX_FRAMES * 4;
        if (vkCreateQueryPool(m_gpu.device(), &qpi, nullptr, &m_aaQueryPool) != VK_SUCCESS)
            throw std::runtime_error("failed to create aa query pool!");

        // Performance panel pool: [0,1] shadows, [2,3] scene. With the panel
        // closed not a single query is used.
        qpi.queryCount = MAX_FRAMES * 4;
        if (vkCreateQueryPool(m_gpu.device(), &qpi, nullptr, &m_perfQueryPool) != VK_SUCCESS)
            throw std::runtime_error("failed to create perf query pool!");
    }

    // The multisample targets of the scene and of the composition. They do NOT belong to the
    // resolve pass: the MSAA resolve happens inside those two render
    // passes, which belong to the Renderer.
    void Renderer::createMsaaImages()
    {
        if (m_aaActiveMode != AaMode::Msaa) return;

        // Multisample image: GpuResources::createImage sets samples = 1, so
        // these two are done by hand. Neither is ever sampled or blitted: they only
        // serve as attachments and are resolved inside the render pass.
        auto createMsImage = [&](VkFormat format, VkImage& image, VkDeviceMemory& memory, VkImageView& view)
        {
            VkImageCreateInfo ii{};
            ii.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
            ii.imageType     = VK_IMAGE_TYPE_2D;
            ii.extent        = { m_renderExtent.width, m_renderExtent.height, 1 };
            ii.mipLevels     = 1;
            ii.arrayLayers   = 1;
            ii.format        = format;
            ii.tiling        = VK_IMAGE_TILING_OPTIMAL;
            ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            ii.usage         = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
            ii.samples       = m_aaSampleCount;
            ii.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
            if (vkCreateImage(m_gpu.device(), &ii, nullptr, &image) != VK_SUCCESS)
                throw std::runtime_error("failed to create multisample image!");

            VkMemoryRequirements req{};
            vkGetImageMemoryRequirements(m_gpu.device(), image, &req);
            VkMemoryAllocateInfo ai{};
            ai.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            ai.allocationSize  = req.size;
            ai.memoryTypeIndex = m_gpu.findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            if (vkAllocateMemory(m_gpu.device(), &ai, nullptr, &memory) != VK_SUCCESS)
                throw std::runtime_error("failed to allocate multisample image memory!");
            vkBindImageMemory(m_gpu.device(), image, memory, 0);

            m_res.createTextureImageView(image, view, format);
        };

        for (int f = 0; f < MAX_FRAMES; f++)
        {
            // Multisample color of the scene and of the composition. Both are
            // resolved inside their render pass onto the usual one-sample
            // images, so nothing behind them (SSAO,
            // SSR, bloom, UI, blit) finds out they exist.
            createMsImage(kHdrFormat,        m_msaaHdrImage[f], m_msaaHdrMemory[f], m_msaaHdrView[f]);
            createMsImage(m_swapChainFormat, m_msaaLdrImage[f], m_msaaLdrMemory[f], m_msaaLdrView[f]);
        }
    }

    void Renderer::destroyMsaaImages()
    {
        auto destroyImage = [&](VkImage& image, VkDeviceMemory& memory, VkImageView& view)
        {
            if (view)   { vkDestroyImageView(m_gpu.device(), view, nullptr);  view   = VK_NULL_HANDLE; }
            if (image)  { vkDestroyImage(m_gpu.device(), image, nullptr);     image  = VK_NULL_HANDLE; }
            if (memory) { vkFreeMemory(m_gpu.device(), memory, nullptr);      memory = VK_NULL_HANDLE; }
        };

        for (int f = 0; f < MAX_FRAMES; f++)
        {
            destroyImage(m_msaaHdrImage[f], m_msaaHdrMemory[f], m_msaaHdrView[f]);
            destroyImage(m_msaaLdrImage[f], m_msaaLdrMemory[f], m_msaaLdrView[f]);
        }
    }

    void Renderer::setAaMode(AaMode mode)
    {
        if (mode == aaMode()) return;
        setAaModeFlag(mode);
        // Any mode change moves resources: the internal size (SSAA), the
        // number of samples (MSAA), or simply the existence of the
        // intermediate image and of the history. It is rebuilt whole, which is easy to
        // reason about and happens once per user click.
        m_aaResourcesDirty = true;
    }

    void Renderer::setViewportSize(uint32_t width, uint32_t height)
    {
        // Collapsed panel or with a null area: there is nothing to render and creating
        // 0-pixel images is invalid. The previous size is kept.
        if (width == 0 || height == 0) return;
        if (m_viewportExtent.width == width && m_viewportExtent.height == height) return;
        m_viewportExtent   = { width, height };
        // Same path as a mode change: recreate with the GPU idle, at the
        // start of the next frame.
        m_aaResourcesDirty = true;
    }

    void Renderer::setSsaaFactor(float v)
    {
        if (v == ssaaFactor()) return;
        setSsaaFactorFlag(v);
        // The targets' size only changes when SSAA is the active mode.
        if (aaMode() == AaMode::Ssaa) m_aaResourcesDirty = true;
    }

    void Renderer::setShadowResolution(int v)
    {
        if (v == shadowResolution() || v <= 0) return;
        setShadowResolutionFlag(v);
        // It is not redone here: the UI calls this in the middle of a frame, and releasing
        // the shadow map right now would pull it out from under the in-flight
        // command list. It is marked and drawFrame handles it between frames.
        m_shadowResourcesDirty = true;
    }

    void Renderer::rebuildShadowResources()
    {
        m_shadowResourcesDirty = false;

        // The image may be in the previous frame, which has not finished
        // yet. It is a quality setting that is touched once in a blue moon: a
        // full wait comes out cheaper than carrying deferred deletion for this.
        vkDeviceWaitIdle(m_gpu.device());

        m_shadowPass.resizeResources(shadowCtx(), (uint32_t)shadowResolution());

        // And EVERYTHING that pointed at the old view. There are three consumers and all
        // three have to be remembered: each mesh's sets and each character
        // material's (binding 3), which refreshShadowDescriptors redoes,
        // and those of the FOG pass, which takes the view and the sampler in its
        // Context (see fogCtx) and therefore is also left with the dead view.
        //
        // Forgetting the fog left a destroyed imageView in a compute set:
        // startup filled with validation errors on EVERY frame.
        refreshShadowDescriptors();
        // createSets resets its pool before handing out, so calling it again does not
        // exhaust it.
        m_fogPass.createSets(fogCtx());
    }

    void Renderer::refreshShadowDescriptors()
    {
        VkDescriptorImageInfo shadowInfo{};
        shadowInfo.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
        shadowInfo.imageView   = m_shadowPass.view();
        shadowInfo.sampler     = m_shadowPass.sampler();

        // The sampler does NOT change on a resize (resizeResources does not touch it), but
        // it is rewritten all the same: the write is a single one and this way the function is also valid
        // if someday the sampler comes to depend on the size.
        auto writeBinding3 = [&](VkDescriptorSet set)
        {
            if (set == VK_NULL_HANDLE) return;
            VkWriteDescriptorSet w{};
            w.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w.dstSet          = set;
            w.dstBinding      = 3;
            w.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            w.descriptorCount = 1;
            w.pImageInfo      = &shadowInfo;
            vkUpdateDescriptorSets(m_gpu.device(), 1, &w, 0, nullptr);
        };

        // Static meshes: per SHARED ENTRY, which is who owns the
        // sets (several objects with the same mesh share one).
        for (int index : m_sharedMeshes.liveIndices())
        {
            SharedGpuMesh* mesh = m_sharedMeshes.get(index);
            if (!mesh) continue;
            for (int f = 0; f < MAX_FRAMES; f++) writeBinding3(mesh->descriptorSets[f]);
        }

        // Characters: one block per material, which is how they are drawn.
        for (SkinnedRenderObject& obj : m_skinnedObjects)
            for (SkinnedMatGfx& mgfx : obj.matGfx)
                for (int f = 0; f < MAX_FRAMES; f++) writeBinding3(mgfx.descSets[f]);
    }

    void Renderer::setMsaaSamples(int v)
    {
        if (v == msaaSamples()) return;
        setMsaaSamplesFlag(v);
        if (aaMode() == AaMode::Msaa) m_aaResourcesDirty = true;
    }

    int Renderer::maxMsaaSamples() const
    {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(m_gpu.physicalDevice(), &props);
        // Color and depth have to match: the scene pass uses
        // both at once, so the useful maximum is the intersection.
        const VkSampleCountFlags counts = props.limits.framebufferColorSampleCounts
                                        & props.limits.framebufferDepthSampleCounts;
        if (counts & VK_SAMPLE_COUNT_8_BIT) return 8;
        if (counts & VK_SAMPLE_COUNT_4_BIT) return 4;
        if (counts & VK_SAMPLE_COUNT_2_BIT) return 2;
        return 1;
    }

    VkSampleCountFlagBits Renderer::targetSampleCount() const
    {
        if (m_aaActiveMode != AaMode::Msaa) return VK_SAMPLE_COUNT_1_BIT;
        const int s = std::min(msaaSamples(), maxMsaaSamples());
        switch (s)
        {
        case 8:  return VK_SAMPLE_COUNT_8_BIT;
        case 4:  return VK_SAMPLE_COUNT_4_BIT;
        case 2:  return VK_SAMPLE_COUNT_2_BIT;
        default: return VK_SAMPLE_COUNT_1_BIT;
        }
    }

    bool Renderer::needsAaIntermediate() const
    {
        return m_aaActiveMode == AaMode::Fxaa || m_aaActiveMode == AaMode::Ssaa || m_aaActiveMode == AaMode::Taa;
    }

    bool Renderer::updateRenderExtent()
    {
        const VkExtent2D before = m_renderExtent;

        if (m_aaActiveMode == AaMode::Ssaa && ssaaFactor() > 1.0f)
        {
            VkPhysicalDeviceProperties props{};
            vkGetPhysicalDeviceProperties(m_gpu.physicalDevice(), &props);
            // maxFramebufferWidth/Height are never smaller than maxImageDimension2D,
            // so clamping by this limit is enough for both.
            const uint32_t limit = props.limits.maxImageDimension2D;

            const uint32_t w = (uint32_t)std::lround(effectiveViewport().width  * (double)ssaaFactor());
            const uint32_t h = (uint32_t)std::lround(effectiveViewport().height * (double)ssaaFactor());
            m_renderExtent.width  = std::min(w, limit);
            m_renderExtent.height = std::min(h, limit);
        }
        else
        {
            m_renderExtent = effectiveViewport();
        }

        return before.width != m_renderExtent.width || before.height != m_renderExtent.height;
    }

    void Renderer::recreateMsaaDependentPipelines()
    {
        // Only those that live in the scene pass and in the composition one. The shadow
        // ones, the depth pre-pass ones and the AA resolve ones have
        // their own render passes that always run at one sample.
        // The four outline ones are NOT in this list: they are owned by
        // SelectionOutlinePass and released below with its destroyResources, which
        // also leaves them at VK_NULL_HANDLE so that createPipeline and
        // createSkinnedGraphicsPipelines redo them clean.
        VkPipeline* pipelines[] = {
            &m_pipeline, &m_wireframePipeline,
            &m_skinnedGfxPipeline, &m_skinnedWireframePipeline,
            &m_compositePipeline,
            // The three skinning compute ones are NO LONGER here: they live in
            // SkinningPass, do not depend on the samples and their creation no longer goes
            // in the same pass as these, so there is no need to destroy or
            // redo them when MSAA changes.
        };
        for (VkPipeline* p : pipelines)
        {
            if (*p != VK_NULL_HANDLE)
            {
                vkDestroyPipeline(m_gpu.device(), *p, nullptr);
                *p = VK_NULL_HANDLE;
            }
        }

        m_outlinePass.destroyResources(outlineCtx());

        createPipeline();
        createSkinnedGraphicsPipelines();

        // The composition pipeline lives in createBloomPipelines, which also
        // creates layouts and pools; here only the pipeline is needed, so it is
        // redone by hand with the same code that one uses.
        recreateCompositePipeline();
        // The SCREEN UI is not redone here: it has its own pass, always at one sample,
        // and the scene's sample count does not affect it.
        //
        // The WORLD one is, and it is mandatory: its two variants are compiled
        // against m_offscreenRenderPass, which the caller (rebuildAaResources)
        // just destroyed and created again with another sample count. A
        // pipeline pointing at a destroyed VkRenderPass gives NO validation
        // error; it shows up as device lost the first time it is used.
        // This is the ONLY recreation path: createOffscreenRenderPass() is only
        // called in two places (init and that block of rebuildAaResources), and
        // recreateSwapChain does not touch it.
        m_uiBatch.initWorldPipelines(m_gpu, m_offscreenRenderPass, m_aaSampleCount);

        // The two that do not live in Renderer.cpp. The skybox is skipped only if there is
        // no cubemap loaded.
        m_skybox.recreatePipeline(m_gpu, m_offscreenRenderPass, m_aaSampleCount);
        Gizmos::recreatePipeline(m_gpu, m_compositeRenderPass, m_aaSampleCount);
    }

    void Renderer::rebuildAaResources()
    {
        m_aaResourcesDirty = false;

        // What was requested becomes what was built BEFORE touching anything: everything
        // below (targetSampleCount, needsAaIntermediate, updateRenderExtent,
        // createAaImages) decides what to create by looking at these two. From here on
        // they match until the next click or the next drag of the panel.
        m_aaActiveMode   = aaMode();
        m_viewportActive = m_viewportExtent;

        // None of this can be touched with work in flight: they are images, render
        // passes and pipelines the GPU may be reading right now.
        vkDeviceWaitIdle(m_gpu.device());

        const VkSampleCountFlagBits wanted = targetSampleCount();
        const bool samplesChanged = (wanted != m_aaSampleCount);

        destroyOffscreenImages();

        if (samplesChanged)
        {
            m_aaSampleCount = wanted;
            // The render passes declare the sample count in each
            // attachment, and the pipelines have to match their pass.
            vkDestroyRenderPass(m_gpu.device(), m_offscreenRenderPass, nullptr);
            vkDestroyRenderPass(m_gpu.device(), m_compositeRenderPass, nullptr);
            createOffscreenRenderPass();
            createCompositeRenderPass();
            recreateMsaaDependentPipelines();
        }

        // The depth is shared by the scene pass and the composition one, so it
        // changes with both the size (SSAA) and the samples (MSAA).
        vkDestroyImageView(m_gpu.device(), m_depthImageView, nullptr);
        vkDestroyImage(m_gpu.device(), m_depthImage, nullptr);
        vkFreeMemory(m_gpu.device(), m_depthImageMemory, nullptr);

        updateRenderExtent();
        createDepthResources();
        createOffscreenImages();

        // The objects' descriptor sets reference the AO map, which
        // was just recreated with another size.
        refreshSsaoDescriptors();
    }

}
