#include "DonTopo/Renderer/Renderer.h"
#include "DonTopo/Renderer/Gizmos.h"
#include "DonTopo/Core/GameObject.h"
#include "DonTopo/Core/Scene.h"
#include "DonTopo/Core/CameraComponent.h"
#include <GLFW/glfw3.h>
#include <stdexcept>
#include "DonTopo/Core/Window.h"
#include <algorithm>
#include <fstream>
#include "DonTopo/Renderer/Vertex.h"
#include <glm/gtc/matrix_transform.hpp>
#include "DonTopo/Renderer/UniformBufferObject.h"
#include "DonTopo/Renderer/SkinnedMeshPacking.h"
#include <limits>
#include <cmath>
#include <chrono>

namespace DonTopo {

    Renderer::~Renderer()
    {
        shutdown();
    }

    // En headless no hay editor que pulse Play: el runtime arranca jugando
    // desde el frame 0.
    bool Renderer::isPlaying() const { return m_headless || (m_ui && m_ui->isPlaying()); }

    void Renderer::init(Window& window, const std::vector<Mesh>& meshes)
    {
        // Se mantiene como la suma de las dos fases, en el mismo orden que
        // antes (ver initPresentation/initSceneResources para el detalle del
        // reparto). El editor (Sandbox) llama a init() y no cambia; el
        // runtime llama a las dos fases por separado para colar el splash
        // entre medias.
        initPresentation(window);
        initSceneResources(meshes);
    }

    void Renderer::initPresentation(Window& window)
    {
        // Gizmos::kFramesInFlight se usa para dimensionar buffers por frame en vuelo
        // dentro de Gizmos; debe coincidir siempre con Renderer::MAX_FRAMES. MAX_FRAMES
        // es private, así que este static_assert vive aquí (contexto de miembro) en vez
        // de a nivel de archivo.
        static_assert(Gizmos::kFramesInFlight == MAX_FRAMES,
            "Gizmos::kFramesInFlight debe coincidir con Renderer::MAX_FRAMES");
        // Mismo caso: MotionBlurPass dimensiona sus imagenes y sus sets por
        // frame en vuelo sin poder ver MAX_FRAMES.
        static_assert(MotionBlurPass::kFramesInFlight == MAX_FRAMES,
            "MotionBlurPass::kFramesInFlight debe coincidir con Renderer::MAX_FRAMES");
        static_assert(FogPass::kFramesInFlight == MAX_FRAMES,
            "FogPass::kFramesInFlight debe coincidir con Renderer::MAX_FRAMES");
        static_assert(SsrPass::kFramesInFlight == MAX_FRAMES,
            "SsrPass::kFramesInFlight debe coincidir con Renderer::MAX_FRAMES");
        static_assert(AaPass::kFramesInFlight == MAX_FRAMES,
            "AaPass::kFramesInFlight debe coincidir con Renderer::MAX_FRAMES");
        static_assert(SsaoPass::kFramesInFlight == MAX_FRAMES,
            "SsaoPass::kFramesInFlight debe coincidir con Renderer::MAX_FRAMES");
        static_assert(DepthPrepassPass::kFramesInFlight == MAX_FRAMES,
            "DepthPrepassPass::kFramesInFlight debe coincidir con Renderer::MAX_FRAMES");

        // Fase 1: lo minimo para poder presentar un frame (splash incluido).
        // El auto-fit de cámara y los recursos de escena (pipelines, shadow,
        // compute, descriptores independientes de la UI como offscreen, mallas)
        // viven en initSceneResources porque dependen de `meshes` o de
        // recursos creados ahí mismo.
        m_gpu.init(window.getNativeWindow());
        // ANTES de cualquier render pass, imagen o pipeline: el numero de
        // muestras del modo de AA activo entra en la creacion de los tres. Sin
        // esto un modo por defecto distinto de None se construiria a una muestra
        // y el MSAA no haria nada, en silencio.
        m_aaActiveMode  = aaMode();
        m_aaSampleCount = targetSampleCount();
        createSwapChain(window);

        createImageViews();
        createDepthResources();
        createOffscreenRenderPass();
        createCompositeRenderPass();
        createUiRenderPass();
        // Los gizmos van en el pass de composicion, no en el de escena: ahi el
        // color ya esta tonemapeado y sus lineas salen exactamente con el color
        // plano que declaran, igual que antes del bloom.
        Gizmos::init(m_gpu, m_compositeRenderPass, m_swapChainFormat, m_aaSampleCount);
        // Passes del AA: solo dependen de m_swapChainFormat, igual que el de
        // composicion, asi que sobreviven a los resize (lo que se recrea son sus
        // imagenes y framebuffers, en createAaImages).
        m_aaPass.createRenderPasses(aaCtx());
        createRenderPass();
        createFramebuffers();
        // createCommandBuffers/createSyncObjects solo dependen del device y
        // del command pool (createCommandBuffers) o del device y
        // m_swapChainImages.size() (createSyncObjects) — nada de
        // initSceneResources (descriptor sets, pipelines, malla) los toca
        // durante el init. Se adelantan aquí, respecto al original, para que
        // queden listos en la fase 1 junto con el resto de lo necesario para
        // presentar.
        createCommandBuffers();
        createSyncObjects();
        // necesita m_renderPass + m_swapChainImages.size(); no depende de
        // nada de initSceneResources, así que se mueve aquí (antes vivía a
        // mitad del init original) para que el splash pueda dibujar con
        // la UI ya operativa si hiciera falta.
        if (!m_headless && m_ui)
        {
            // Los handles viajan como enteros: UiLayer ya no incluye vulkan.h,
            // porque el mismo editor tiene que poder dibujarse con DirectX 12.
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
        // Suelo del rango: una escena diminuta (o con todo en el mismo punto)
        // daría far ~0 y no se vería ni el skybox. 200 deja far=600, que cubre de
        // sobra la cámara con la que abre el editor (z=300 mirando al origen), y
        // near=0.2, que no recorta props pequeños.
        constexpr float kMinCameraDistance = 200.0f;

        glm::vec3 bMin( std::numeric_limits<float>::max());
        glm::vec3 bMax(-std::numeric_limits<float>::max());
        bool      any = false;

        for (const RenderObject& obj : m_objects)
        {
            const SharedGpuMesh* gpu = m_sharedMeshes.get(obj.sharedIndex);
            if (!gpu || !gpu->hasBounds) continue;

            // Las 8 esquinas de la AABB local llevadas a mundo: con el objeto
            // rotado o escalado, la caja alineada a ejes de la malla ya no acota.
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
            // La cota de un skinned es una esfera en local (vale para toda pose):
            // se toma su centro en mundo y el radio sin escalar. Aproximado a
            // propósito — esto solo fija near/far, no culea nada.
            const glm::vec3 center(obj.transform[3]);
            bMin = glm::min(bMin, center - glm::vec3(obj.boundRadius));
            bMax = glm::max(bMax, center + glm::vec3(obj.boundRadius));
            any = true;
        }

        // Nada acotable: conserva el rango vigente en vez de dejarlo en infinitos.
        // Es lo que pasa con la escena vacía de un proyecto recién creado.
        if (!any) return;

        m_cameraTarget = (bMin + bMax) * 0.5f;
        const float maxDim =
            glm::max(bMax.x - bMin.x, glm::max(bMax.y - bMin.y, bMax.z - bMin.z));
        m_cameraDistance = glm::max(maxDim * 1.2f, kMinCameraDistance);
    }

    void Renderer::initSceneResources(const std::vector<Mesh>& meshes)
    {
        // Auto-fit camera to mesh bounding box (necesita `meshes`; por eso
        // vive aquí y no en initPresentation).
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

        // Sin una sola malla con vertices -escena vacia, o todas vacias- los
        // extremos se quedan tal cual salieron (bMin en +max y bMax en -max), y
        // entonces maxDim vale -inf. De ahi pasa a m_cameraDistance y al rango
        // de profundidad que se deriva de el, y el editor arranca con una
        // proyeccion de infinitos que no dibuja nada. refitCameraRange() si se
        // guarda de este caso; este camino no lo hacia.
        if (bMin.x > bMax.x)
        {
            bMin = glm::vec3(-1.0f);
            bMax = glm::vec3( 1.0f);
        }

        m_cameraTarget   = (bMin + bMax) * 0.5f;
        float maxDim     = glm::max(bMax.x - bMin.x, glm::max(bMax.y - bMin.y, bMax.z - bMin.z));
        m_cameraDistance = maxDim * 1.2f;

        // ANTES de createPipeline y createShadowResources: los dos pipeline
        // layouts declaran m_instanceDescLayout como set 1.
        createInstanceResources();
        createDescriptorSetLayout();
        // ANTES de createPipeline: el pipeline layout de escena declara
        // m_fpDescLayout como set 2. Los buffers que SI dependen del tamano (la
        // rejilla) los crea despues createFpBuffers, desde createOffscreenImages.
        // Los timestamps: propiedades del device que comparten todos los pases
        // con queries. Este pass corre ANTES que createBloomPipelines (el layout
        // del pipeline de escena necesita el set de aqui), asi que se resuelven
        // aqui mismo; el bloom volvera a leer exactamente lo mismo.
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
        // ANTES de createDescriptorSets: los sets de cada objeto escriben ya las
        // vistas de los dos cubemaps del IBL (bindings 5 y 6). Aqui se crean con
        // contenido neutro; initSkybox los rellenara si hay entorno.
        m_iblPass.createResources(iblCtx());
        // ANTES de createOffscreenImages: ahi se crea la cadena de mips, que
        // necesita el descriptor set layout y el pool del bloom ya montados.
        createBloomPipelines();
        // UI de juego: mismo pass y mismas muestras que la composicion, que es
        // donde se graban sus lotes (LDR, ya tonemapeado, encima de la escena).
        // Pass propio y UNA muestra: la UI ya no va dentro de la composicion,
        // asi que ni la toca el AA ni depende del numero de muestras de la
        // escena (y por eso tampoco hay que recrear su pipeline al cambiar MSAA).
        m_uiBatch.init(m_gpu, m_res, m_uiRenderPass, VK_SAMPLE_COUNT_1_BIT);
        // Los canvas de MUNDO no van en ese pass: se graban dentro del de
        // ESCENA, con la perspectiva de la camara y tapados por la geometria.
        // Sus dos variantes se compilan contra m_offscreenRenderPass y con SUS
        // muestras, que no son las del pass de UI. Hay que rehacerlas cada vez
        // que se recrea ese renderpass — ver recreateMsaaDependentPipelines().
        m_uiBatch.initWorldPipelines(m_gpu, m_offscreenRenderPass, m_aaSampleCount);
        // ANTES de createOffscreenImages (que llama a createSsaoImages) y DESPUÉS
        // de ShadowPass::createResources: el pipeline del depth pre-pass
        // reutiliza el pipeline layout del pass de sombras, que se crea allí.
        m_depthPrepass.createRenderPassAndPipeline(depthPrepassCtx());
        m_ssaoPass.createPipelines(ssaoCtx());
        // Detrás del SSAO: comparte su sampler de profundidad (DepthPrepassPass) y
        // su depth pre-pass, y el pool de queries se apoya en el
        // m_timestampsSupported que resolvió el bloom.
        m_ssrPass.createPipelines(ssrCtx());
        // Detras del SSR: come del MISMO depth pre-pass y del mismo sampler de
        // profundidad, y su pool de queries se apoya en el mismo
        // m_timestampsSupported.
        m_fogPass.createPipelines(fogCtx());
        // Detras de la niebla, y ANTES de createOffscreenImages: sus imagenes y
        // sus descriptor sets se crean con el swapchain y necesitan el layout y
        // el pool ya montados.
        m_motionBlurPass.createPipeline(motionBlurCtx());
        // ANTES de createOffscreenImages (que llama a createAaImages): ahi se
        // alojan los descriptor sets del AA, que necesitan sus layouts y sus
        // pools ya montados. El pool de queries se apoya en el
        // m_timestampsSupported que resolvio el bloom.
        m_aaPass.createPipelines(aaCtx());
        createAaQueryPools();
        // La capa de UI ya se inicializó en initPresentation. En editor,
        // createOffscreenImages necesita que lo esté (llama a
        // registerUiTexture); en headless no la llama, así que el orden no
        // importa. Como initUi corrió antes (fase 1) y esta llamada corre en
        // fase 2, el orden UI→offscreen se conserva igual que en el init
        // original.
        createOffscreenImages();

        m_objects.resize(meshes.size());
        for(size_t i = 0; i < meshes.size(); i++)
        {
            buildRenderObject(meshes[i], m_objects[i]);
        }

        createUniformBuffers();
        createDescriptorPool();
        createDescriptorSets();
        // DETRAS de createUniformBuffers: el set de la niebla referencia el UBO
        // del frame, y en el init ese buffer todavia no existia cuando corrio
        // createOffscreenImages. En las recreaciones por swapchain ya existe y
        // los sets los rehace createOffscreenImages.
        m_fogPass.createSets(fogCtx());
    }

    bool Renderer::beginSplash(const std::string& logoPath)
    {
        // Sobre el render pass del swapchain ya creado por initPresentation
        // (createRenderPass): color-only, un solo attachment (VK_FORMAT =
        // m_swapChainFormat, sin depth) — ver el comentario "solo color,
        // usados por el pass de UI" en createFramebuffers. El pipeline del
        // splash (Task 3) se crea con pDepthStencilState = nullptr, que es
        // compatible con este render pass precisamente porque no tiene
        // attachment de depth/stencil. No lanza si el logo falta.
        return m_splash.init(m_gpu, m_renderPass, m_swapChainFormat, logoPath);
    }

    void Renderer::drawSplashFrame(float alpha)
    {
        if (!m_splash.isInitialized()) return;

        vkWaitForFences(m_gpu.device(), 1, &m_inFlight[m_currentFrame], VK_TRUE, UINT64_MAX);

        uint32_t imageIndex;
        VkResult res = vkAcquireNextImageKHR(m_gpu.device(), m_swapChain, UINT64_MAX,
            m_imageAvailable[m_currentFrame], VK_NULL_HANDLE, &imageIndex);
        if (res == VK_ERROR_OUT_OF_DATE_KHR) return; // durante el splash no recreamos: el siguiente frame lo hara
        if (res != VK_SUCCESS && res != VK_SUBOPTIMAL_KHR) return;

        vkResetFences(m_gpu.device(), 1, &m_inFlight[m_currentFrame]);
        vkResetCommandBuffer(m_commandBuffers[m_currentFrame], 0);

        VkCommandBufferBeginInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        vkBeginCommandBuffer(m_commandBuffers[m_currentFrame], &bi);

        VkClearValue clear{};
        clear.color = { { 0.05f, 0.05f, 0.06f, 1.0f } }; // mismo fondo que el shader

        VkRenderPassBeginInfo rp{};
        rp.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rp.renderPass        = m_renderPass;
        rp.framebuffer       = m_swapChainFramebuffers[imageIndex];
        rp.renderArea.extent = m_swapChainExtent;
        rp.clearValueCount   = 1; // m_renderPass es color-only (createRenderPass, 1 attachment)
        rp.pClearValues      = &clear;
        vkCmdBeginRenderPass(m_commandBuffers[m_currentFrame], &rp, VK_SUBPASS_CONTENTS_INLINE);

        // Viewport/scissor dinamicos (el pipeline los declara dinamicos).
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
        // 1. Espera a que el frame anterior terminó
        vkWaitForFences(m_gpu.device(), 1, &m_inFlight[m_currentFrame], VK_TRUE, UINT64_MAX);

        // 2. Pide la siguiente imagen del swapchain
        uint32_t imageIndex;
        VkResult result;

        result = vkAcquireNextImageKHR(m_gpu.device(), m_swapChain, UINT64_MAX, m_imageAvailable[m_currentFrame], VK_NULL_HANDLE, &imageIndex);
        if(result == VK_ERROR_OUT_OF_DATE_KHR)
        {
            recreateSwapChain(window);
            // Este frame no llega a grabar/dibujar comandos: limpiar aquí evita
            // que los vértices de gizmos acumulados por drawX(...) antes de esta
            // llamada se arrastren duplicados al siguiente frame que sí dibuje.
            Gizmos::clear();
            return;
        }

        if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
        {
            throw std::runtime_error("failed to acquire next image!");
        }        

        vkResetFences(m_gpu.device(), 1, &m_inFlight[m_currentFrame]);

        // 3. Graba el command buffer
        if(vkResetCommandBuffer(m_commandBuffers[m_currentFrame], 0) != VK_SUCCESS)
        {
            throw std::runtime_error("failed to reset command buffer!");
        }

        // Un cambio de modo de AA, de sus parametros de recursos o del tamano del
        // panel del viewport se resuelve AQUI: la fence de este frame ya
        // senalizo y el command buffer aun no se ha grabado, asi que es el punto
        // donde se pueden destruir imagenes y pipelines sin pillar trabajo en
        // vuelo. Y ANTES de buildUiFrame, que es lo importante: la
        // reconstruccion re-registra la textura del viewport y le da un
        // VkDescriptorSet nuevo. Si corriera despues, ImGui ya habria grabado el
        // viejo -recien destruido- en la lista de dibujo de este frame.
        if (m_aaResourcesDirty) rebuildAaResources();
        // Mismo sitio y mismo motivo: entre frames y con la GPU parada.
        if (m_shadowResourcesDirty) rebuildShadowResources();

        // Reflection probes: MISMO sitio y mismo motivo que la linea de arriba.
        // Aqui se puede esperar a que la GPU quede libre para bakear una sonda o
        // reescribir los bindings 5/6 de un descriptor set. Sin sondas en la
        // escena sale por el camino rapido sin tocar nada, y con sondas ya
        // bakeadas y quietas tampoco graba un solo comando: el coste GPU por
        // frame es identico en los tres casos.
        m_probePass.sync(probeCtx());

        // ── Construir frame de UI (antes de grabar el command buffer) ─────────────
        // En headless no hay capa de UI que alimentar: el runtime blitea
        // la imagen offscreen directamente al swapchain (ver recordCommandBuffer).
        if (!m_headless && m_ui)
            m_ui->buildUiFrame(m_offscreenDescSet[m_currentFrame], m_sceneRoot, m_viewMatrix);

        // Se muestrea AQUÍ, después de buildUiFrame() y no antes: ese draw()
        // es quien puede voltear m_isPlaying (botones Play/Stop) o mutar la
        // escena (Add/Remove/Create Camera). currentFrameCamera() lee ambas
        // cosas, así que si se llamaba antes del draw(), el UBO (geometría
        // iluminada) y el command buffer grabado justo debajo (skybox +
        // gizmos) podían acabar leyendo cámaras distintas en el frame exacto
        // del clic — un frame de tearing visible. El UBO está en memoria
        // host-mapeada: basta con escribirlo antes del vkQueueSubmit de más
        // abajo, no hace falta que sea lo primero del frame.
        //
        // Las cascadas se ajustan al frustum de la cámara, así que se calculan
        // aquí, con la misma cámara ya estable, y ANTES de los dos que las
        // consumen: updateUniformBuffer (que las copia al UBO) y
        // recordCommandBuffer (que culea y graba el pass de sombras con ellas).
        // Forward+: se congela AQUI, despues de buildUiFrame (que es quien puede
        // haber cambiado el modo con el combo) y antes de los dos que lo
        // consumen. Sin este punto unico, el bloque de parametros que lee
        // pbr.frag y el dispatch que se graba podrian salir de modos distintos en
        // el frame exacto del clic.
        m_fpActiveMode = m_fpMode;

        // La camara del frame se muestrea aqui y no dentro del pase: es el mismo
        // currentFrameCamera() que ve el culling del pass de sombras.
        {
            // El centro de la escena, a donde apunta una luz de punto. Se saca
            // AQUI, junto a las cascadas, porque es el mismo valor que tiene que
            // ver la niebla (via fogCtx) en este frame.
            SceneCenter centro;
            for (const RenderObject& object : m_objects)      centro.add(object.transform);
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

        // 4. Envía a la GPU
        // En headless el pass 2 no dibuja UI: blitea la imagen offscreen al
        // swapchain (recordCommandBuffer), así que el primer uso real de la
        // imagen adquirida ocurre en TRANSFER, no en COLOR_ATTACHMENT_OUTPUT.
        // Esperar el semáforo también en TRANSFER hace que esa ordenación sea
        // local y explícita, en vez de depender de que la barrera del blit se
        // encadene con trabajo previo del pass 1 (ver el comentario largo del
        // srcStageMask en recordCommandBuffer, que sigue vigente y explica por
        // qué ESA barrera no puede esperar solo en TRANSFER). Añadir un stage
        // al wait solo puede hacer que la GPU espere más, nunca menos: no
        // cambia nada del camino con editor.
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

        // 5. Presenta
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

        // Limpia los vértices de gizmos ya subidos/dibujados este frame, para
        // que el siguiente ciclo de drawX(...) (llamado por el caller ANTES de
        // invocar drawFrame) empiece desde un buffer vacío.
        Gizmos::clear();
        m_currentFrame = (m_currentFrame + 1) % MAX_FRAMES;
    }

    void Renderer::shutdown()
    {
        if (m_gpu.device() == VK_NULL_HANDLE) return;
        vkDeviceWaitIdle(m_gpu.device());

        // El vkDeviceWaitIdle de arriba es la precondición de flushAll: sin él,
        // esto destruiría recursos que la GPU todavía puede estar leyendo.
        m_deferredDeletes.flushAll(m_gpu.device());

        destroyOffscreenImages();
        if (!m_headless && m_ui) m_ui->shutdownUi();
        vkDestroyRenderPass(m_gpu.device(), m_offscreenRenderPass, nullptr);
        m_offscreenRenderPass = VK_NULL_HANDLE;
        // Bloom + composicion. Las imagenes y los sets ya se han ido con
        // destroyOffscreenImages (llama a destroyBloomImages); aqui quedan los
        // objetos que no dependen del tamano del swapchain.
        vkDestroyPipeline(m_gpu.device(), m_compositePipeline, nullptr);
        vkDestroyPipelineLayout(m_gpu.device(), m_compositePipelineLayout, nullptr);
        vkDestroyDescriptorPool(m_gpu.device(), m_compositeDescPool, nullptr);
        vkDestroyDescriptorSetLayout(m_gpu.device(), m_compositeDescLayout, nullptr);
        vkDestroyRenderPass(m_gpu.device(), m_compositeRenderPass, nullptr);
        m_compositeRenderPass = VK_NULL_HANDLE;
        // El de la UI no depende del numero de muestras, asi que solo se
        // destruye aqui, en el teardown de verdad.
        if (m_uiRenderPass != VK_NULL_HANDLE)
        {
            vkDestroyRenderPass(m_gpu.device(), m_uiRenderPass, nullptr);
            m_uiRenderPass = VK_NULL_HANDLE;
        }
        m_bloomPass.destroyPipelines(bloomCtx());
        // SSAO. Las imagenes, vistas, framebuffers y sets se fueron con
        // destroyOffscreenImages (llama a destroySsaoImages); aqui queda lo que
        // no depende del tamano. El pipeline layout es el del pass de sombras y
        // se destruye con el, mas abajo.
        m_ssaoPass.destroyPipelines(ssaoCtx());
        m_depthPrepass.destroyRenderPassAndPipeline(depthPrepassCtx());

        // SSR: las imagenes y los sets ya se fueron con destroyOffscreenImages;
        // aqui solo queda lo que es independiente del tamano.
        m_ssrPass.destroyPipelines(ssrCtx());

        // Niebla volumetrica: no tiene imagen ni sampler propios (escribe dentro
        // del HDR y muestrea con el sampler del SSAO y el del shadow map), asi
        // que aqui esta todo lo suyo menos los sets.
        m_fogPass.destroyPipelines(fogCtx());

        // Motion blur: aqui lo que no depende del tamano. Las imagenes y los
        // sets se fueron con destroyImages.
        m_motionBlurPass.destroyPipeline(motionBlurCtx());

        // Forward+: la rejilla y la lista de indices se fueron con
        // destroyOffscreenImages; aqui queda lo que no depende del tamano. Los
        // tres buffers mapeados en persistente no necesitan unmap: el mapeo muere
        // con la memoria, igual que en el UBO y en el SSBO de instancias.
        m_fpPass.destroyPipelines(fpCtx());

        // Anti-aliasing: las imagenes, los framebuffers y los sets se fueron con
        // destroyOffscreenImages (llama a destroyAaImages); aqui queda lo que no
        // depende del tamano ni del modo.
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
        vkDestroyPipeline(m_gpu.device(), m_outlinePipeline, nullptr);
        vkDestroyPipeline(m_gpu.device(), m_outlineWirePipeline, nullptr);
        vkDestroyPipelineLayout(m_gpu.device(), m_pipelineLayout, nullptr);
        vkDestroyRenderPass(m_gpu.device(), m_renderPass, nullptr);
        for(VkImageView imageView : m_swapChainImageViews)
        {
            vkDestroyImageView(m_gpu.device(), imageView, nullptr);
        }                        
        vkDestroySwapchainKHR(m_gpu.device(), m_swapChain, nullptr);
        // La cadena m_descriptorPools se destruye más abajo, DESPUÉS de los dos bucles que
        // llaman a destroyRenderObject y destroySkinnedRenderObject: ambas
        // funciones liberan sets del pool (creado con
        // FREE_DESCRIPTOR_SET_BIT pa soportar rebuildSkinnedMesh), y destruir
        // el pool aquí antes dejaría un handle ya destruido al que liberar.
        m_objects.clear();
        // Sin refcounts ni diferido: el vkDeviceWaitIdle de arriba garantiza que
        // nadie está leyendo, y a estas alturas ya no queda quien dibuje.
        m_sharedMeshes.destroyAll([this](const SharedGpuMesh& gpu) {
            destroySharedGpuMesh(gpu);
        });
        for(int i = 0; i < MAX_FRAMES; i++)
        {
            vkDestroyBuffer(m_gpu.device(), m_uniformBuffers[i], nullptr);
            vkFreeMemory(m_gpu.device(), m_uniformBuffersMemory[i], nullptr);
        }
        vkDestroyDescriptorSetLayout(m_gpu.device(), m_descriptorSetLayout, nullptr);
        // SSBO de instancias: los sets salen con el pool (sin
        // FREE_DESCRIPTOR_SET, viven todo el proceso).
        for (int i = 0; i < MAX_FRAMES; i++)
            destroyInstanceBuffer(i);
        vkDestroyDescriptorPool(m_gpu.device(), m_instanceDescPool, nullptr);
        vkDestroyDescriptorSetLayout(m_gpu.device(), m_instanceDescLayout, nullptr);
        vkDestroyImageView(m_gpu.device(), m_depthImageView, nullptr);
        vkDestroyImage(m_gpu.device(), m_depthImage, nullptr);
        vkFreeMemory(m_gpu.device(), m_depthImageMemory, nullptr);
        // Shadow map. El Context lleva los dos set layouts que ya se han
        // destruido cuatro lineas mas arriba; destroyResources no los toca (un
        // pipeline layout sobrevive a los set layouts con los que se creo).
        m_shadowPass.destroyResources(shadowCtx());
        // Las sondas ANTES del IBL global: destroy() de las sondas no toca los
        // pipelines de convolucion, pero si el orden se invirtiera un futuro
        // camino de limpieza con convolucion pendiente se quedaria sin ellos.
        m_probePass.destroy(probeCtx());
        m_iblPass.destroyResources(iblCtx());
        vkDestroyPipeline(m_gpu.device(), m_skinnedGfxPipeline, nullptr);
        vkDestroyPipeline(m_gpu.device(), m_skinnedWireframePipeline, nullptr);
        vkDestroyPipeline(m_gpu.device(), m_skinnedOutlinePipeline, nullptr);
        vkDestroyPipeline(m_gpu.device(), m_skinnedOutlineWirePipeline, nullptr);
        for (auto& obj : m_skinnedObjects)
        {
            destroySkinnedRenderObject(obj);
        }

        m_skinnedObjects.clear();
        // Ahora sí: ya no queda ningún destroySkinnedRenderObject pendiente que
        // necesite liberar sets de la cadena de pools.
        for (VkDescriptorPool pool : m_descriptorPools)
        {
            if (pool != VK_NULL_HANDLE)
                vkDestroyDescriptorPool(m_gpu.device(), pool, nullptr);
        }
        m_descriptorPools.clear();
        // Igual que el de arriba: ya no queda ningun destroySkinnedRenderObject
        // pendiente que necesite liberar sets del pool de compute.
        m_skinningPass.destroyPipelines(skinningCtx());
        m_skybox.shutdown(m_gpu);
        m_splash.shutdown(m_gpu);
        Gizmos::shutdown(m_gpu);
        // Los atlas ANTES del batch: sus descriptor sets salen de su pool, y
        // destruir el pool primero dejaria los handles colgando.
        for (auto& atlas : m_uiAtlases) atlas->destroy(m_gpu);
        m_uiAtlases.clear();
        // Punteros a lo que se acaba de destruir: fuera antes de que nadie los
        // pueda volver a pedir.
        m_uiAtlasByPath.clear();
        m_uiAtlasImGuiId.clear();
        for (auto& font : m_uiFonts) font->destroy(m_gpu);
        m_uiFonts.clear();
        m_uiBatch.shutdown(m_gpu);
        printf("destroy render items OK\n"); fflush(stdout);
        m_gpu.shutdown();
    }

    UiTextureAtlas* Renderer::loadUiAtlas(const std::string& path)
    {
        // La misma ruta dos veces es el mismo atlas: el editor lo consulta para
        // enseñar sus sprites y el sync lo pide cada vez que cambia un widget.
        if (auto it = m_uiAtlasByPath.find(path); it != m_uiAtlasByPath.end())
            return it->second;

        auto atlas = std::make_unique<UiTextureAtlas>();
        if (!atlas->loadFromFile(m_gpu, m_res, path)) return nullptr;
        // Sub-rects, si los hay: sin sidecar el atlas se usa entero, que es lo
        // que hacia siempre. No es un fallo que no este.
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
        // Sin editor no hay a quién registrarla, y sin vista no hay nada subido.
        if (!atlas || !m_ui || atlas->view() == VK_NULL_HANDLE) return 0;

        if (auto it = m_uiAtlasImGuiId.find(atlas); it != m_uiAtlasImGuiId.end())
            return it->second;

        const uint64_t id = m_ui->registerUiTexture((uint64_t)m_uiBatch.sampler(),
                                                    (uint64_t)atlas->view());
        m_uiAtlasImGuiId[atlas] = id;
        return id;
    }

    UiFont* Renderer::loadUiFont(const std::string& path, float bakePx)
    {
        auto font = std::make_unique<UiFont>();
        if (!font->loadFromFile(m_gpu, m_res, path, bakePx)) return nullptr;
        // La fuente CONTIENE su atlas, asi que el registro del descriptor es
        // exactamente el mismo que el de un atlas de sprites.
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
        // Empareja por ownerId: los slots que sobreviven conservan su árbol y
        // su caché, así que reordenar los canvas en la jerarquía no reconstruye
        // lo que no ha cambiado (eso se vería como un parpadeo).
        matchUiCanvasSlots(bindings, m_uiSlots);

        for (size_t i = 0; i < bindings.size(); i++)
        {
            UiCanvasSlot& s = *m_uiSlots[i];
            const UiCanvasBinding& b = bindings[i];
            if (b.canvas) b.canvas->applyTo(s.canvas);
            const UiCanvasRenderMode modo =
                b.canvas ? b.canvas->renderMode : UiCanvasRenderMode::ScreenSpace;
            // Cambiar de modo mete o saca el canvas del reparto de input
            // (screenCanvasesTopFirst filtra por ScreenSpace). Si se va a World a
            // media pulsación —renderMode es escribible desde Lua— nunca ve el
            // MouseUp y se queda con su captura y su hover: al volver le robaría
            // el puntero al de encima durante un frame. releaseInput lo suelta.
            if (modo != s.mode) s.canvas.releaseInput();
            s.mode      = modo;
            s.depthTest = b.canvas ? b.canvas->depthTest  : true;
            // Copia por valor de los ajustes de mundo y del transform del
            // GameObject: la matriz de modelo se calcula al GRABAR (necesita la
            // vista de la camara para el billboard) y para entonces el binding
            // ya no existe. Sin canvas se queda el componente por defecto, que
            // no se llega a leer porque el modo sera ScreenSpace.
            if (b.canvas) s.component = *b.canvas;
            s.worldTransform = b.worldTransform;
            syncUiWidgets(b.widgets, s.canvas, s.cache, *this);
        }
    }

    UiCanvas& Renderer::uiCanvas()
    {
        // El PRIMER canvas de pantalla, en el orden de la escena: es el mismo
        // criterio que usaba el shim temporal (Task 4), así que un proyecto con
        // un solo canvas de pantalla se ve exactamente igual que antes.
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
        // El orden sale de la MISMA función libre que usa D3D12: el de más
        // arriba primero, o sea el pase de UI (que recorre m_uiSlots en orden)
        // al revés. Duplicar el criterio aquí es como los dos backends se
        // desincronizan.
        screenCanvasesTopFirst(m_uiSlots, out);
    }

    const UiCanvas* Renderer::uiCanvasOf(uint64_t ownerId) const
    {
        // Misma funcion libre que D3D12: un solo criterio de busqueda.
        return findCanvasByOwner(m_uiSlots, ownerId);
    }

    const UiElement* Renderer::findUiNode(const std::string& name) const
    {
        // TODOS los canvas, no solo el de pantalla: un botón de un canvas de
        // mundo también tiene que poder llevar gizmo en el editor.
        for (const auto& s : m_uiSlots)
        {
            if (!s) continue;
            if (const UiElement* n = findUiNodeIn(s->canvas.root(), name)) return n;
        }
        return nullptr;
    }

    void Renderer::initSkybox(const std::array<std::string, 6>& facePaths)
    {
        // kHdrFormat: el skybox dibuja en el pass de escena, que desde el bloom
        // sale en flotante. Su color pasa por el tonemap de la composicion como
        // el resto de la escena — y es lo que permite que el cielo genere bloom.
        m_skybox.init(m_gpu, m_offscreenRenderPass, kHdrFormat, facePaths, m_aaSampleCount);
        // El cubemap recien cargado es la fuente del IBL. Una sola vez, aqui: es
        // el punto por el que pasan tanto el editor como DonTopoRuntime, y no
        // depende de nada del editor.
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

        // Edición: la proyección de siempre (45° fijos + near/far derivados de
        // m_cameraDistance). No se toca a propósito — el componente solo manda
        // en Play, así que el editor no cambia de look.
        FrameCamera fc{ m_viewMatrix,
                        glm::perspective(glm::radians(45.0f), aspect,
                                          m_cameraDistance * 0.001f, m_cameraDistance * 3.0f),
                        m_camera.getPos() };
        fc.proj[1][1] *= -1.0f; // Vulkan Y flip

        // Play con cámara en escena: manda el CameraComponent. m_camera y
        // m_viewMatrix NO se tocan nunca — siguen siendo los del editor, así que
        // al parar Play la vista vuelve sola, sin guardar ni restaurar estado
        // (y sin que main.cpp, que llama a setCamera cada frame, se entere).
        // Sin cámara en escena se cae al repliegue de arriba; el aviso al Log lo
        // da EditorUI al arrancar Play, no aquí (esto corre cada frame).
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

    // Las tres delegan: la geometria del culling vive en Renderer/Frustum.h y
    // Renderer/SkinnedBounds.h, fuera de este fichero y de Vulkan. Los
    // wrappers se quedan porque son la puerta por la que entran los tests y el
    // resto de este fichero.
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

        // Un driver que dice soportar la superficie pero no da ni un formato
        // deja el vector vacio, y la eleccion de mas abajo arranca leyendo
        // surfaceFormats[0]: acceso fuera de rango en el arranque, sin
        // diagnostico. Es un caso que no deberia pasar, y por eso mismo hay que
        // decirlo en vez de leer basura.
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
        // El tamano interno del render sale de este: igual salvo en SSAA, donde
        // es este por el factor. Tiene que quedar fijado ANTES de crear el depth
        // y los targets intermedios, que ya van a la resolucion interna.
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
        // TRANSFER_DST: en modo headless la imagen offscreen se blitea aquí en
        // vez de dibujarse la UI encima. El flag va incondicional para que
        // editor y runtime compartan el mismo camino de creación de recursos.
        createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        createInfo.preTransform = surfaceCapabilities.currentTransform;
        createInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        createInfo.presentMode = VK_PRESENT_MODE_FIFO_KHR; // vsync
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
            // Mapeo de canales (identidad = sin cambios)
            createInfo.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
            createInfo.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
            createInfo.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
            createInfo.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
            // Qué parte de la imagen usamos
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

    // Pass de escena 3D → offscreen (finalLayout=SHADER_READ para que la UI lo muestree)
    void Renderer::createOffscreenRenderPass()
    {
        VkAttachmentDescription colorAtt{};
        // HDR y no m_swapChainFormat: aqui sale la escena SIN tonemapear (ver el
        // final de pbr.frag), asi que el attachment tiene que aguantar valores
        // por encima de 1.0 o el umbral del bloom no encontraria nada.
        colorAtt.format         = kHdrFormat;
        colorAtt.samples        = m_aaSampleCount;
        colorAtt.loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
        // Con MSAA lo que se conserva es el RESUELTO, no el multisample: este
        // attachment no lo lee nadie despues, asi que guardarlo seria pagar el
        // ancho de banda de N muestras para tirarlas.
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

        // Destino del resolve: m_hdrImage, la imagen de UNA muestra de siempre.
        // Sale en SHADER_READ_ONLY igual que sin MSAA, asi que el SSAO, el SSR,
        // el bloom y la composicion leen exactamente lo mismo que leian.
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
        // STORE y ya no DONT_CARE: el pass de composicion carga esta misma
        // profundidad para que el contorno de seleccion y los gizmos sigan
        // teniendo contra que testear despues de haberse mudado alli.
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
        // Sin MSAA no hay nada que resolver y el puntero se queda nulo, que es
        // como estuvo este pass hasta ahora.
        if (m_aaSampleCount != VK_SAMPLE_COUNT_1_BIT)
            subpass.pResolveAttachments = &resolveRef;

        // Dependencias: garantizan que el bloom (compute) y la composicion
        // (fragment) pueden leer la textura cuando el pass acaba.
        VkSubpassDependency deps[2]{};
        deps[0].srcSubpass    = VK_SUBPASS_EXTERNAL;
        deps[0].dstSubpass    = 0;
        // COMPUTE tambien en el src: el lector de la imagen HDR ya no es solo el
        // fragment shader de la composicion, tambien el downsample del bloom del
        // frame anterior, y esta dependencia es la que impide pisarla.
        deps[0].srcStageMask  = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
        deps[0].dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        deps[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        deps[0].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

        deps[1].srcSubpass    = 0;
        deps[1].dstSubpass    = VK_SUBPASS_EXTERNAL;
        // El depth se escribe en LATE_FRAGMENT_TESTS y ahora lo lee el pass de
        // composicion, asi que entra en el srcStageMask junto al color.
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
        // Un solo attachment: la imagen final LDR, con lo que ya haya dentro.
        // Una muestra SIEMPRE, aunque la escena vaya con MSAA: aqui ya no hay
        // geometria 3D que suavizar, y el propio AA ya ha resuelto.
        VkAttachmentDescription colorAtt{};
        colorAtt.format         = m_swapChainFormat;
        colorAtt.samples        = VK_SAMPLE_COUNT_1_BIT;
        // LOAD y no DONT_CARE: debajo de la UI esta la escena entera.
        colorAtt.loadOp         = VK_ATTACHMENT_LOAD_OP_LOAD;
        colorAtt.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
        colorAtt.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        colorAtt.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        // Entra y sale en SHADER_READ_ONLY: es como la dejan los dos caminos
        // que escriben antes (composicion sin AA, o el pass de resolucion) y
        // como la esperan el panel del editor y el blit headless.
        colorAtt.initialLayout  = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        colorAtt.finalLayout    = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkAttachmentReference colorRef{};
        colorRef.attachment = 0;
        colorRef.layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments    = &colorRef;

        // Lo de antes escribio color (composicion o resolucion del AA); esto
        // vuelve a escribir sobre lo mismo, asi que la dependencia va de salida
        // de color a salida de color.
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
        // Color: la imagen offscreen LDR de siempre (formato del swapchain), la
        // que muestrea la UI y la que blitea el runtime headless. El triangulo de
        // pantalla completa la cubre entera, asi que no hace falta cargar ni
        // limpiar nada previo.
        VkAttachmentDescription colorAtt{};
        colorAtt.format         = m_swapChainFormat;
        colorAtt.samples        = m_aaSampleCount;
        colorAtt.loadOp         = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        // Con MSAA lo que se conserva es el resuelto, igual que en el pass de
        // escena: el color multisample no lo lee nadie.
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

        // Destino del resolve del MSAA: m_offscreenImage. Que este pass sea
        // tambien multisample es lo que hace que el contorno de seleccion y los
        // gizmos salgan suavizados: se rasterizan a N muestras contra el depth
        // multisample de la escena. Resolver el depth para dibujarlos en un pass
        // de una muestra no es opcion, VK_KHR_depth_stencil_resolve es Vulkan 1.2.
        VkAttachmentDescription resolveAtt = colorAtt;
        resolveAtt.samples      = VK_SAMPLE_COUNT_1_BIT;
        resolveAtt.loadOp       = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        resolveAtt.storeOp      = VK_ATTACHMENT_STORE_OP_STORE;
        resolveAtt.finalLayout  = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkAttachmentReference resolveRef{};
        resolveRef.attachment = 2;
        resolveRef.layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        // Depth: el MISMO buffer que acaba de escribir el pass de escena, cargado
        // tal cual. Es lo que permite que el contorno y los gizmos se dibujen
        // aqui, ya en LDR, sin que el tonemap les toque el color y respetando
        // exactamente la profundidad de la escena.
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
        // Entrada: espera al pass de escena (color+depth) y a los dispatches del
        // bloom, que son las dos fuentes que este pass muestrea.
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

        // Salida: la UI (o el blit headless) lee la imagen ya compuesta.
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

    // Pass de UI → swapchain (solo color, sin depth)
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

    // Framebuffers del swapchain: solo color, usados por el pass de UI
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
                // m_imageAvailable — señala que hay imagen disponible del swapchain
                // m_renderFinished — señala que el render terminó, listo para presentar
                // m_inFlight — fence que bloquea la CPU hasta que la GPU terminó ese frame
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
        if (m_outlineStaticIndex < 0 && m_outlineSkinnedIndex < 0)
            return;

        // Grosor del casco relativo al tamaño del objeto en mundo: con un valor
        // fijo, un objeto grande apenas mostraría borde y uno pequeño quedaría
        // engullido por él.
        constexpr float kOutlineFactor = 0.009f;
        auto maxWorldScale = [](const glm::mat4& m)
        {
            return glm::max(glm::length(glm::vec3(m[0])),
                   glm::max(glm::length(glm::vec3(m[1])), glm::length(glm::vec3(m[2]))));
        };

        if (m_outlineStaticIndex >= 0 && (size_t)m_outlineStaticIndex < m_objects.size())
        {
            const RenderObject&  obj = m_objects[m_outlineStaticIndex];
            const SharedGpuMesh* gpu = m_sharedMeshes.get(obj.sharedIndex);
            // Mismas guardas que el bucle de dibujo, en el mismo orden: checkbox
            // "Visible", entrada borrada, upload en vuelo y frustum. Un objeto
            // sin contorno porque está fuera de cámara —u oculto— es lo correcto:
            // el objeto tampoco se ha dibujado.
            bool visible = obj.meshVisible && gpu && gpu->uploadTicket <= m_lastCompletedTicket;
            if (visible && gpu->hasBounds &&
                !aabbVisible(camFrustum, gpu->aabbMin, gpu->aabbMax, obj.transform))
            {
                visible = false;
            }

            if (visible)
            {
                const glm::vec3 extent = gpu->aabbMax - gpu->aabbMin;
                const float maxExtent  = glm::max(extent.x, glm::max(extent.y, extent.z));

                PushData push;
                push.transform = obj.transform;
                // flags.x = 0: el outline dibuja UNA instancia con su matriz
                // aquí, no por el SSBO — ni siquiera lo lee outline.vert.
                push.flags.y = glm::max(maxExtent * maxWorldScale(obj.transform), 1.0f) * kOutlineFactor;

                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    isWireframeMode() ? m_outlineWirePipeline : m_outlinePipeline);
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

        if (m_outlineSkinnedIndex >= 0 && (size_t)m_outlineSkinnedIndex < m_skinnedObjects.size())
        {
            // La visibilidad ya la decidió el culling del principio del frame:
            // es la misma que gobernó el compute de skinning, así que si vale 0
            // el buffer de salida ni siquiera se ha actualizado y dibujar el
            // casco sacaría una pose vieja.
            // Con el checkbox "Visible" apagado pasa exactamente lo mismo: el
            // compute no se despacha, así que no hay pose que dibujar.
            if (m_skinnedVisible[m_outlineSkinnedIndex] &&
                m_skinnedObjects[m_outlineSkinnedIndex].meshVisible)
            {
                const SkinnedRenderObject& sobj = m_skinnedObjects[m_outlineSkinnedIndex];
                // matGfx vacío: no habría de dónde sacar el set 0, y el UBO que
                // lee outline.vert vive ahí. Sin materiales no hay contorno.
                if (sobj.outputVertexBuffer != VK_NULL_HANDLE && !sobj.matGfx.empty())
                {
                    PushData push;
                    push.transform = sobj.transform;
                    push.flags.y = glm::max(sobj.restMaxExtent * maxWorldScale(sobj.transform), 1.0f)
                                   * kOutlineFactor;

                    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                        isWireframeMode() ? m_skinnedOutlineWirePipeline : m_skinnedOutlinePipeline);
                    // Un solo draw sobre todo el index buffer: los submeshes solo
                    // existen para cambiar de material, y el contorno es de un
                    // color plano.
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
        // Al apagar se invalidan los slots pendientes: sus queries no se van a
        // volver a resetear, y leerlas al reabrir el panel devolveria basura del
        // frame en que se cerro (o NOT_READY para siempre).
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

    void Renderer::recordCommandBuffer(uint32_t imageIndex)
    {
        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        if(vkBeginCommandBuffer(m_commandBuffers[m_currentFrame], &beginInfo) != VK_SUCCESS)
        {
            throw std::runtime_error("failed to begin command buffer!");
        }

        // SSBO de instancias del frame: se dimensiona ANTES de grabar nada,
        // porque crecerlo recrea el buffer y actualiza su descriptor set (aquí
        // es seguro: drawFrame ya esperó la fence de este frame). El peor caso
        // es que todos los objetos sean visibles en TODOS los passes: uno por
        // cascada del shadow map más el de la escena, de ahí el factor
        // SHADOW_CASCADES + 1. El cursor arranca a 0: las cascadas escriben
        // delante (una detrás de otra), la escena al final.
        // +2 y no +1: además del pass de escena, el depth pre-pass del SSAO
        // escribe su propio tramo del SSBO con el conjunto visible de la cámara.
        ensureInstanceCapacity((uint32_t)m_objects.size() * (SHADOW_CASCADES + 2));
        m_instanceCursor = 0;

        // Cámara del frame: la usan el culling de los skinned (aquí abajo), el
        // de los estáticos y, más adelante en el pass principal, el skybox y los
        // gizmos. Se muestrea UNA vez para que todos vean exactamente la misma.
        const FrameCamera fc = currentFrameCamera();
        const Frustum camFrustum = frustumFromViewProj(fc.proj * fc.view);

        // Visibilidad de los skinned ANTES del compute: es la misma decisión que
        // lee el bucle de dibujo, así que el frame en que un personaje vuelve a
        // entrar en cámara se le despacha el skinning y se dibuja después, en
        // este mismo command buffer, con su animTime actual. Separar las dos
        // decisiones sería lo que dejaría un frame con la pose vieja.
        //
        // El pass de sombras sólo itera m_objects, así que los skinned no
        // proyectan sombra y basta con el frustum de la cámara.
        m_skinnedVisible.assign(m_skinnedObjects.size(), 0);
        for (size_t i = 0; i < m_skinnedObjects.size(); i++)
        {
            const SkinnedRenderObject& sobj = m_skinnedObjects[i];
            if (sobj.outputVertexBuffer == VK_NULL_HANDLE) continue; // borrado desde el editor
            // En vuelo: su SSBO de entrada y sus texturas se subieron en un batch
            // cuya fence no ha señalado. Ni compute ni dibujo hasta que complete.
            if (sobj.uploadTicket > m_lastCompletedTicket) continue;
            if (sobj.hasBounds &&
                !aabbVisible(camFrustum, glm::vec3(-sobj.boundRadius), glm::vec3(sobj.boundRadius),
                             sobj.transform))
            {
                continue;
            }
            m_skinnedVisible[i] = 1;
        }

        // ── Medida del anti-aliasing ──────────────────────────────────────────
        // Cuatro queries por frame en vuelo: [0,1] el pass propio del modo (lo
        // escribe recordAaPass) y [2,3] el render completo sin UI, que se mide
        // SIEMPRE, tambien sin AA. Los resultados que se leen aqui son los de
        // hace dos frames en este mismo slot: la fence ya los espero.
        if (m_timestampsSupported && m_aaQueryPending[m_currentFrame])
        {
            uint64_t total[2] = {};
            if (vkGetQueryPoolResults(m_gpu.device(), m_aaQueryPool, m_currentFrame * 4 + 2, 2,
                                      sizeof(total), total, sizeof(uint64_t),
                                      VK_QUERY_RESULT_64_BIT) == VK_SUCCESS)
            {
                m_renderGpuMs = (float)((double)(total[1] - total[0]) * m_timestampPeriod * 1e-6);
            }
            // El par del pass propio se lee aparte y solo si ese frame llego a
            // escribirlo: en None y en MSAA no existe, y pedir las cuatro de
            // golpe devolveria NOT_READY para todas y se perderia tambien el
            // total de arriba.
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
                printf("aa (%s, %ux muestras): pass propio %.3f ms, render completo %.3f ms (%ux%u interno, %ux%u ventana)\n",
                       kNames[(int)m_aaActiveMode], (uint32_t)m_aaSampleCount, m_aaGpuMs, m_renderGpuMs,
                       m_renderExtent.width, m_renderExtent.height,
                       m_swapChainExtent.width, m_swapChainExtent.height);
                fflush(stdout);
            }
        }
        if (m_timestampsSupported)
        {
            // Reset unico de las cuatro: recordAaPass ya no puede resetear por su
            // cuenta sin machacar el par del total.
            vkCmdResetQueryPool(m_commandBuffers[m_currentFrame], m_aaQueryPool, m_currentFrame * 4, 4);
            vkCmdWriteTimestamp(m_commandBuffers[m_currentFrame], VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                m_aaQueryPool, m_currentFrame * 4 + 2);
        }

        // ── Medida del panel Performance ─────────────────────────────────────
        // Solo si el panel esta abierto. La lectura es del slot de hace dos
        // frames (la fence de este frame ya lo espero), sin WAIT_BIT: si el
        // driver aun no las tiene se conserva el valor anterior y ya.
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
        // ANTES del pass de escena: pbr.frag necesita el AO ya resuelto, y el AO
        // necesita la profundidad de TODA la escena. Por eso el depth pre-pass, y
        // por eso va aquí y no después.
        recordSsaoPass(m_commandBuffers[m_currentFrame], camFrustum, fc.proj);
        // Forward+: DETRAS del pre-pass (el tiled lee esa profundidad) y DELANTE
        // del pass de escena, que es quien consume la rejilla de luces. En Off no
        // graba ni un comando.
        m_fpPass.record(fpCtx(), m_commandBuffers[m_currentFrame], fc.proj);

        // ── UI: draw data de TODOS los canvas del frame, y beginFrame ───────────
        // Aqui arriba y no dentro del pase de UI, que es donde vivia. El motivo
        // es que los canvas de MUNDO se graban en el pase de ESCENA, que empieza
        // tres lineas mas abajo: si beginFrame() siguiera en el pase de UI, los
        // de mundo llamarian a record() ANTES de que nadie hubiera dimensionado
        // el buffer ni reiniciado los cursores. La capacidad seria la del frame
        // anterior (o 0) y la guarda uiCursorFits los descartaria EN SILENCIO —
        // ni un error, ni un aviso de validacion, ni un canvas en pantalla.
        //
        // Y por eso el total tiene que ser el de TODOS los canvas del frame, de
        // mundo y de pantalla: un solo buffer compartido, un solo beginFrame.
        // Llamarlo otra vez para el pase de UI reiniciaria los cursores y los
        // canvas de pantalla pisarian los vertices de los de mundo, que la GPU
        // todavia no ha leido (lee el buffer al EJECUTAR, no al grabar).
        //
        // Construir el draw data aqui obliga a conocer ya el espacio de cada
        // canvas, y se conoce: el de pantalla es effectiveViewport(), y el de
        // mundo es su propia referenceResolution (en modo World el canvas no se
        // ajusta a ninguna pantalla, lo fija CanvasComponent::applyTo).
        const VkExtent2D uiExtent = effectiveViewport();
        uint32_t uiTotalVertices = 0;
        uint32_t uiTotalIndices  = 0;
        // Los de PANTALLA aparte: son los unicos que abren el pase de UI. Con el
        // total del frame se abriria tambien con solo canvas de mundo vivos, y
        // seria un vkCmdBeginRenderPass sin un solo draw dentro.
        uint32_t uiScreenVertices = 0;
        uint32_t uiScreenIndices  = 0;
        for (auto& s : m_uiSlots)
        {
            if (!s) continue;
            if (s->mode == UiCanvasRenderMode::World)
            {
                // La matriz de MODELO, aqui y no en syncUiCanvases: necesita la
                // vista de la camara para el billboard, y syncUiCanvases no la
                // tiene. Aqui ademas cae ANTES de sortWorldCanvasesBackToFront,
                // que ordena leyendo la posicion de model[3] — calcularla ya en
                // el grabado la dejaria a cero para el orden.
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

        // ── Pass 1: escena 3D → offscreen ────────────────────────────────────────
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
            // Las guardas y el culling siguen siendo POR OBJETO: se resuelven
            // aquí, que es donde está la caché GPU, y el agrupado solo ve el
            // resultado en `visible`. Agrupar antes de cullear dibujaría de más.
            m_batchCandidates.clear();
            m_batchCandidates.reserve(m_objects.size());
            for (auto& obj : m_objects)
            {
                const SharedGpuMesh* gpu = m_sharedMeshes.get(obj.sharedIndex);
                // !gpu: borrado desde el editor. uploadTicket por delante del
                // último completado: todavía en vuelo, sus texturas siguen en
                // TRANSFER_DST_OPTIMAL y samplearlas sería leer basura; aparece
                // en cuanto la fence de su batch señale.
                // obj.meshVisible: checkbox "Visible" del componente Mesh. Mismo
                // filtro en los pases de sombra y de AO: oculto no llega a la GPU
                // en ninguno, así que no proyecta sombra ni ocluye.
                bool visible = obj.meshVisible && gpu && gpu->uploadTicket <= m_lastCompletedTicket;
                // Fuera de cámara: no gasta ni slot en el SSBO. Los objetos sin
                // AABB (mesh vacío) pasan siempre.
                if (visible && gpu->hasBounds &&
                    !aabbVisible(camFrustum, gpu->aabbMin, gpu->aabbMax, obj.transform))
                {
                    visible = false;
                }
                // La fuerza de SSR entra en la clave del agrupado: es una push
                // constant por grupo, igual que metallic y roughness, así que dos
                // objetos con la misma malla y distinta fuerza no pueden ir en el
                // mismo draw. Con el SSR global apagado todos entran a 0 y el
                // agrupado sale exactamente igual que antes de la feature.
                const float ssr = m_ssrEnabled ? obj.ssrStrength : 0.0f;
                m_batchCandidates.push_back({ obj.sharedIndex, visible, &obj.transform, ssr });
            }

            // El pass de sombras ya ha escrito su parte del buffer: sus
            // transforms van delante y los de aquí detrás, con el cursor como
            // base de los firstInstance.
            const uint32_t instanceBase = m_instanceCursor;
            glm::mat4* dst = (glm::mat4*)m_instanceMapped[m_currentFrame] + instanceBase;
            m_instanceCursor += buildInstanceBatches(m_batchCandidates.data(), m_batchCandidates.size(),
                dst, m_instanceCapacity[m_currentFrame] - instanceBase, instanceBase, m_instanceBatches);

            // Contadores del panel Performance: se cuentan sobre los mismos
            // candidatos que acaba de agrupar buildInstanceBatches, asi que
            // reflejan exactamente lo que se va a dibujar abajo.
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

            // Set 1 una sola vez para todo el pass: el SSBO no cambia entre
            // draws, y el pipeline skinned de abajo comparte layout, así que
            // sigue bindeado y válido también para él.
            vkCmdBindDescriptorSets(m_commandBuffers[m_currentFrame], VK_PIPELINE_BIND_POINT_GRAPHICS,
                m_pipelineLayout, 1, 1, &m_instanceDescSets[m_currentFrame], 0, nullptr);

            // Forward+: uno por frame y comun a TODOS los draws del pass (estatico
            // y skinned), asi que se bindea una sola vez aqui. Va DENTRO del pass
            // de escena y no antes: el pass de sombras y el depth pre-pass usan
            // el pipeline layout de ShadowPass, que solo declara dos sets, y
            // bindear con el deja el set 2 sin definir.
            const VkDescriptorSet fpSceneSet = m_fpPass.set(m_currentFrame);
            if (fpSceneSet != VK_NULL_HANDLE)
            {
                vkCmdBindDescriptorSets(m_commandBuffers[m_currentFrame], VK_PIPELINE_BIND_POINT_GRAPHICS,
                    m_pipelineLayout, 2, 1, &fpSceneSet, 0, nullptr);
            }

            for (const InstanceBatch& batch : m_instanceBatches)
            {
                // No puede ser nullptr: solo llegan a un grupo los candidatos
                // que ya pasaron la guarda de arriba.
                const SharedGpuMesh* gpu = m_sharedMeshes.get(batch.sharedIndex);
                vkCmdBindDescriptorSets(m_commandBuffers[m_currentFrame], VK_PIPELINE_BIND_POINT_GRAPHICS,
                    m_pipelineLayout, 0, 1, &gpu->descriptorSets[m_currentFrame], 0, nullptr);
                PushData push;
                // transform se queda en la identidad: con useInstancing = 1 el
                // vertex shader coge el model matrix del SSBO. metallic y
                // roughness siguen aquí porque son por ENTRADA compartida (no
                // por instancia): son constantes dentro del grupo y pbr.frag ya
                // los lee de este mismo bloque.
                push.metallic  = gpu->metallic;
                push.roughness = gpu->roughness;
                push.flags.x   = 1.0f;
                // pbr.frag la vuelca al alfa del HDR, que es la máscara por píxel
                // que lee ssr.comp.
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
                    // Borrado, en vuelo o fuera de cámara: la decisión ya la tomó
                    // el culling del principio del frame, la misma que decidió si
                    // se le despachaba el compute.
                    if (!m_skinnedVisible[si])
                    {
                        if (perfStamp) m_statCulled++;
                        continue;
                    }
                    SkinnedRenderObject& sobj = m_skinnedObjects[si];
                    // Checkbox "Visible" del componente Mesh. Va aquí y no en
                    // m_skinnedVisible porque ese flag también gobierna el
                    // despacho del compute (que sigue corriendo: el contorno de
                    // selección lee sus vértices) y el contorno mismo.
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
                        // flags.x se queda a 0 (ruta skinned, matriz propia).
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

            // Contorno del objeto seleccionado: después de toda la geometría
            // (necesita el depth buffer completo para que el casco solo asome
            // por el borde) y antes del skybox.
            // El contorno de selección y los gizmos YA NO se dibujan aquí: se han
            // mudado al pass de composición, que es LDR. Este pass sale sin
            // tonemapear y les habría cambiado su color plano.

            // Proyección del skybox (mismo pass, misma cámara que el culling de
            // arriba). El Y-flip ya viene aplicado desde currentFrameCamera().
            // Con jitter cuando el modo es TAA: tiene que moverse EXACTAMENTE
            // igual que la geometría o el TAA vería un borde permanente entre
            // ambos. Fuera de TAA es fc.proj tal cual.
            const glm::mat4 proj = m_aaPass.jitteredProj();

            // Skybox — fullscreen quad, depth LEQUAL sin escritura (al final del pass).
            // Omitido en wireframe: el fondo ya es negro sólido (clearValue por defecto).
            if (!isWireframeMode() && m_skybox.isInitialized()) {
                glm::mat4 rotView    = glm::mat4(glm::mat3(fc.view)); // sin traslación
                glm::mat4 invViewProj = glm::inverse(proj * rotView);
                m_skybox.draw(m_commandBuffers[m_currentFrame], invViewProj);
            }

            // ── Canvas de MUNDO ──────────────────────────────────────────────
            // Lo ultimo del pase, detras de la geometria y del skybox: van con
            // alpha y tienen que mezclarse sobre lo que ya hay. Aqui —y no en el
            // pase de UI— es lo que les da perspectiva y lo que hace que una
            // pared los tape: el depth buffer de la escena esta cargado y la
            // variante con depthTest lo lee (escribir no escribe ninguna).
            //
            // `proj` es la JITTEREADA (la misma que el skybox y que la
            // geometria): con TAA, un canvas de mundo sin jitter dejaria un
            // borde permanente contra todo lo que si lo lleva.
            //
            // LIMITACION CONOCIDA — los post que reconstruyen posicion desde la
            // profundidad tratan al canvas como la GEOMETRIA QUE TIENE DETRAS.
            // Un canvas de mundo NO entra en el depth pre-pass (ese solo graba
            // mallas) y NO escribe profundidad (depthWrite va apagado en las
            // tres variantes, a proposito: la UI va con alpha). Asi que en el
            // pixel que ocupa, el depth que leen los post es el de lo que hay
            // detras. Los tres afectados, todos muestreando el mismo depthTex
            // del pre-pass:
            //   - fog.comp        -> un cartel cerca de la camara delante de una
            //                        pared lejana recibe la niebla DE LA PARED:
            //                        sale sobre-nublado.
            //   - motion_blur.comp-> recibe los vectores de movimiento de la
            //                        pared: arrastra al mover la camara.
            //   - taa.frag        -> reproyecta con el depth de la pared.
            // No se arregla aqui: meterlos en el pre-pass les daria oclusion de
            // AO y romperia el alpha. Al verificar en GUI hay que mirar los
            // colores con la NIEBLA APAGADA primero, o se confunde el
            // sobre-nublado con un fallo de la conversion sRGB->lineal.
            //
            // El orden lo pone la funcion libre de UiWidgetSync.h, que es la
            // que esta probada sin GPU (test_world_canvases_se_ordenan_de_lejos_a_cerca).
            // El draw data y la matriz de modelo ya estan hechos arriba, antes
            // del beginFrame.
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
                // Las cuatro estan escritas: el slot ya es legible dentro de dos
                // frames. Si la captura se apaga antes, setPerfCaptureEnabled
                // limpia el flag y no se lee un pool sin resetear.
                m_perfQueryPending[m_currentFrame] = true;
            }
        }

        // SSR: necesita el color de la escena YA iluminado, así que va DETRÁS del
        // pass de escena; y suma el reflejo dentro del propio HDR ANTES del bloom,
        // para que el reflejo genere bloom y pase por el tonemap ACES igual que el
        // resto de la imagen. Con el efecto apagado no graba nada y el HDR se
        // queda exactamente como salió del render pass.
        m_ssrPass.record(ssrCtx(), m_commandBuffers[m_currentFrame], fc.proj);

        // Niebla volumetrica: detrás del SSR (quiere el color ya iluminado y con
        // los reflejos dentro) y antes del bloom, para que el in-scattering
        // florezca y pase por el tonemap ACES igual que el resto de la imagen.
        // Apagada no graba nada.
        m_fogPass.record(fogCtx(), m_commandBuffers[m_currentFrame], fc.view, fc.proj);

        // Motion blur de cámara: detrás de la niebla (emborrona la imagen tal y
        // como se va a ver) y antes del bloom, para que la estela arrastre los
        // highlights y florezca con ellos. Apagado no graba nada.
        m_motionBlurPass.record(motionBlurCtx(), m_commandBuffers[m_currentFrame]);

        // ── Bloom + composición: HDR → tonemap → offscreen LDR ───────────────────
        {
            VkCommandBuffer cmd = m_commandBuffers[m_currentFrame];

            if (bloomEnabled())
            {
                m_bloomPass.beginQuery(bloomCtx(), cmd);
                m_bloomPass.record(bloomCtx(), cmd);
            }
            else
            {
                // Apagado: ni un dispatch de la cadena, y sin timestamps que medir.
                // El slot deja de tener par pendiente para que al reencender no se
                // lea una medida de antes del apagón.
                m_bloomPass.skipQuery(bloomCtx());
                m_bloomPass.recordClear(bloomCtx(), cmd);
            }

            VkRenderPassBeginInfo rpInfo{};
            rpInfo.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
            rpInfo.renderPass        = m_compositeRenderPass;
            // Con FXAA, SSAA o TAA la composicion (tonemap + contorno + gizmos)
            // va a la imagen intermedia y el pass de resolucion la lleva de ahi a
            // m_offscreenImage. En None y en MSAA escribe directamente en
            // m_offscreenImage, exactamente como antes de esta feature: mismo
            // render pass, mismos comandos.
            rpInfo.framebuffer       = needsAaIntermediate() ? m_aaPass.compositeFramebuffer(m_currentFrame)
                                                             : m_compositeFramebuffer[m_currentFrame];
            rpInfo.renderArea.extent = m_renderExtent;
            rpInfo.renderArea.offset = {0, 0};
            // Los dos attachments son DONT_CARE/LOAD: nada que limpiar.
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

            // Sin cadena de mips (viewport minúsculo) o con el efecto apagado no
            // hay nada que sumar: la intensidad se fuerza a 0 y queda solo el
            // tonemap. El pass NO se puede saltar: es quien tonemapea.
            const float intensity = (bloomEnabled() && m_bloomPass.mipCount() > 0) ? m_bloomIntensity : 0.0f;
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_compositePipeline);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_compositePipelineLayout,
                                    0, 1, &m_compositeSets[m_currentFrame], 0, nullptr);
            vkCmdPushConstants(cmd, m_compositePipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT,
                               0, sizeof(float), &intensity);
            vkCmdDraw(cmd, 3, 1, 0, 0);

            // Contorno y gizmos, ya sobre la imagen tonemapeada y con la
            // profundidad de la escena cargada: mismo resultado que cuando vivían
            // en el pass anterior, pero sin pasar por el tonemap ni por el bloom.
            recordSelectionOutline(cmd, camFrustum);
            Gizmos::draw(cmd, fc.proj * fc.view, m_currentFrame);

            vkCmdEndRenderPass(cmd);

            // Solo con el bloom encendido: el par se abre arriba bajo la misma
            // condición, y escribir aquí sin haber reseteado dejaría la query sucia.
            if (m_timestampsSupported && bloomEnabled())
                vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, m_bloomPass.queryPool(), m_currentFrame * 2 + 1);

            // Anti-aliasing: lo ultimo de la cadena de post, sobre color LDR ya
            // tonemapeado y con el contorno de seleccion y los gizmos ya dibujados
            // (asi que tambien se les suavizan los bordes, que es lo deseado: sus
            // lineas y el casco invertido son lo mas escalonado de la pantalla).
            m_aaPass.record(aaCtx(), cmd);

            // ── UI de juego, ya sobre la imagen final ─────────────────────────
            // Despues del AA a proposito: aqui el texto no lo suaviza el FXAA ni
            // lo arrastra el historial del TAA, y con SSAA se dibuja UNA vez al
            // tamano de salida en vez de supersamplearse. Es donde la dibuja
            // tambien el backend de DirectX 12 (sobre el back buffer resuelto).
            //
            // Va por encima de la escena, del contorno de seleccion y de los
            // gizmos, y por debajo de la interfaz del editor, que se graba en el
            // pass del swapchain. Con el canvas vacio no se abre ni el pass.
            //
            // El draw data de TODOS los slots y el beginFrame() del batch ya
            // estan hechos ARRIBA, antes del pase de escena: los canvas de mundo
            // se graban alli y necesitan el buffer dimensionado y los cursores
            // reiniciados antes que nadie. Aqui solo quedan los de PANTALLA, que
            // se graban uno por uno dentro del MISMO vkCmdBeginRenderPass.
            //
            // La condicion de apertura son los de PANTALLA y solo ellos: con el
            // total del frame, un proyecto con unicamente canvas de mundo
            // abriria este pase para no grabar ni un draw dentro.
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

                // El viewport es estado dinamico y este pass es propio: hay que
                // ponerlo, no se hereda del de composicion.
                VkViewport vp{};
                vp.width    = (float)uiExtent.width;
                vp.height   = (float)uiExtent.height;
                vp.minDepth = 0.0f;
                vp.maxDepth = 1.0f;
                vkCmdSetViewport(cmd, 0, 1, &vp);

                // Aqui NO va un beginFrame(). El del frame ya se llamo arriba,
                // antes del pase de escena, con el total de mundo + pantalla.
                // Llamarlo otra vez reiniciaria los cursores a 0 y los canvas de
                // pantalla escribirian ENCIMA de los vertices de los de mundo,
                // que la GPU todavia no ha leido (lee el buffer al EJECUTAR la
                // lista, no al grabarla): los canvas de mundo saldrian con la
                // geometria del de pantalla, sin un solo aviso de validacion.

                // bottom=0 y top=alto: (0,0) cae ARRIBA a la izquierda. Parece del revés
                // y es justo lo contrario: en Vulkan el +Y de NDC va hacia ABAJO, así
                // que la receta de OpenGL (top=0, bottom=alto) deja [1][1] negativo y
                // dibuja la UI ENTERA espejada — invisible mientras solo hubo quads de
                // color, evidente en cuanto se dibujó la primera letra. RH_ZO porque
                // Vulkan clipea z fuera de [0,1] y glm::ortho a secas da [-1,1].
                // Del ESPACIO DEL CANVAS (píxeles de salida), no del framebuffer: los
                // vértices llegan en esos píxeles y el viewport ya estira el NDC al
                // framebuffer entero. Con SSAA eso deja la UI supersampleada en vez de
                // encogida a 1/factor, que es lo que salía al proyectar con el extent
                // del render.
                const glm::mat4 uiProj = glm::orthoRH_ZO(0.0f, (float)uiExtent.width,
                                                         0.0f, (float)uiExtent.height,
                                                         0.0f, 1.0f);

                // Canvas y framebuffer son ya el MISMO espacio (pixeles de
                // salida): los scissor no se escalan.
                for (auto& s : m_uiSlots)
                {
                    if (!s || s->mode != UiCanvasRenderMode::ScreenSpace) continue;
                    m_uiBatch.record(m_gpu, cmd, s->drawData, uiProj, uiExtent, uiExtent, m_currentFrame);
                }
                vkCmdEndRenderPass(cmd);
            }

            // Cierre de la medida del render completo, ya con el AA incluido. Es
            // la referencia con la que se compara el sobrecoste de SSAA y MSAA,
            // que no tienen pass propio que medir.
            if (m_timestampsSupported)
            {
                vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, m_aaQueryPool, m_currentFrame * 4 + 3);
                m_aaQueryPending[m_currentFrame] = true;
            }
        }

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
            // Sin editor no hay quien muestree la imagen offscreen, así que se
            // copia tal cual a la imagen de presentación. Es un blit 1:1: la
            // offscreen se crea con el mismo formato y extent que el swapchain
            // (createOffscreenImages). El renderpass offscreen declara
            // initialLayout=UNDEFINED, así que no hay que restaurar su layout
            // después del blit.
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

            // srcStageMask = COLOR_ATTACHMENT_OUTPUT y no TRANSFER: esto no es un
            // despiste, es lo que ordena esta barrera (y por tanto el blit de abajo)
            // detrás del semáforo de vkAcquireNextImageKHR. El submit de este frame
            // solo espera ese semáforo en COLOR_ATTACHMENT_OUTPUT_BIT
            // (submitInfo.pWaitDstStageMask, más abajo), que no cubre TRANSFER — así
            // que si la barrera esperase únicamente en TRANSFER, el driver no tendría
            // ninguna dependencia que la obligue a ir después de la adquisición de la
            // imagen del swapchain y el blit podría ejecutarse sobre una imagen que
            // aún no es nuestra (o pisar la del frame anterior in-flight). Que
            // funcione depende de que el Pass 1 (offscreen) SIEMPRE emita trabajo en
            // COLOR_ATTACHMENT_OUTPUT antes de esta barrera, así que el srcStageMask
            // de aquí se encadena con ese trabajo y queda correctamente después del
            // semáforo. Si algún día "se corrige" este srcStageMask a
            // VK_PIPELINE_STAGE_TRANSFER_BIT (que es lo que parece obvio a primera
            // vista), se rompe esa cadena en silencio: sin validation layers de por
            // medio no hay ningún aviso, solo un blit ocasionalmente sobre una imagen
            // todavía no adquirida.
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

    void Renderer::createPipeline()
    {
        auto vertCode = loadShaderFile("shaders/triangle.vert.spv");
        auto fragCode = loadShaderFile("shaders/pbr.frag.spv");

        VkShaderModule vertModule = createShaderModule(vertCode);
        VkShaderModule fragModule = createShaderModule(fragCode);

        // 1. Shader stages — qué shader va en cada etapa
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

        // 2. Vertex input — sin vertex buffer, las posiciones van en el shader
        VkVertexInputBindingDescription bindingDesc{};
        bindingDesc.binding     = 0;
        bindingDesc.stride      = sizeof(Vertex);
        bindingDesc.inputRate   = VK_VERTEX_INPUT_RATE_VERTEX;

        // Atributo 0: pos (vec2, offset 0)
        VkVertexInputAttributeDescription attrDescs[5]{};
        attrDescs[0].binding    = 0;
        attrDescs[0].location   = 0;
        attrDescs[0].format     = VK_FORMAT_R32G32B32_SFLOAT;
        attrDescs[0].offset     = offsetof(Vertex, pos);

        // Atributo 1: color (vec3, offset después de pos)
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

        // 3. Input assembly — qué primitivo forman los vértices
        VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
        inputAssembly.sType     = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        inputAssembly.topology  = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

        // 4. Viewport y scissor — dinámicos, los seteamos en recordCommandBuffer
        VkPipelineViewportStateCreateInfo viewportInfo{};
        viewportInfo.sType          = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        viewportInfo.viewportCount  = 1;
        viewportInfo.scissorCount   = 1;

        // 5. Rasterizer — cómo se rellena el triángulo
        VkPipelineRasterizationStateCreateInfo rasterizationInfo{};
        rasterizationInfo.sType         = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rasterizationInfo.polygonMode   = VK_POLYGON_MODE_FILL;
        rasterizationInfo.cullMode      = VK_CULL_MODE_BACK_BIT;
        rasterizationInfo.frontFace     = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rasterizationInfo.lineWidth     = 1.0f;

        // 6. Multisampling — lo fija el modo de AA. Tiene que coincidir con el
        // numero de muestras del render pass del pipeline (escena para los dos
        // primeros, composicion para los dos contornos) o el pipeline es invalido.
        VkPipelineMultisampleStateCreateInfo multisampleInfo{};
        multisampleInfo.sType                   = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisampleInfo.rasterizationSamples    = m_aaSampleCount;

        VkPipelineDepthStencilStateCreateInfo depthStencil{};
        depthStencil.sType                 = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        depthStencil.depthTestEnable       = VK_TRUE;
        depthStencil.depthWriteEnable      = VK_TRUE;
        depthStencil.depthCompareOp        = VK_COMPARE_OP_LESS;
        depthStencil.depthBoundsTestEnable = VK_FALSE;
        depthStencil.stencilTestEnable     = VK_FALSE;

        // 7. Color blending — opaco, sin transparencia
        VkPipelineColorBlendAttachmentState blendAttachment{};
        blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

        VkPipelineColorBlendStateCreateInfo colorBlendInfo{};
        colorBlendInfo.sType            = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        colorBlendInfo.attachmentCount  = 1;
        colorBlendInfo.pAttachments     = &blendAttachment;

        // 8. Dynamic state — viewport y scissor los cambiamos en runtime (no hardcoded aquí)
        VkDynamicState dynamicStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo dynamicInfo{};
        dynamicInfo.sType               = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dynamicInfo.dynamicStateCount   = 2;
        dynamicInfo.pDynamicStates      = dynamicStates;

        // 9. Pipeline layout — sin descriptors ni push constants por ahora
        VkPushConstantRange pushRange{};
        pushRange.stageFlags    = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        pushRange.offset        = 0;
        pushRange.size          = sizeof(PushData);   // 80 bytes

        // Set 0: UBO + texturas (por entrada compartida). Set 1: SSBO de
        // transforms por instancia, uno por frame, que se bindea una vez por
        // pass. Este layout lo comparten el pipeline estático, el wireframe y el
        // skinned; el skinned no lee el set 1 (useInstancing = 0), pero al
        // compartir layout el set sigue bindeado y es válido.
        // Set 2: los buffers de Forward+ (parametros, luces, rejilla e indices).
        // Uno por frame, se bindea una vez por pass. Set propio y no bindings
        // nuevos del set 0 porque ese solo tenia libre el 8 y ampliarlo obligaria
        // a reescribir el descriptor set de CADA objeto. Solo lo lee pbr.frag; el
        // wireframe, el skinned y el outline comparten layout y no lo declaran,
        // que es legal mientras no lo usen.
        VkDescriptorSetLayout setLayouts[] = { m_descriptorSetLayout, m_instanceDescLayout, m_fpPass.descLayout() };

        VkPipelineLayoutCreateInfo layoutInfo{};
        layoutInfo.sType                    = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layoutInfo.setLayoutCount           = 3;
        layoutInfo.pSetLayouts              = setLayouts;
        layoutInfo.pushConstantRangeCount   = 1;
        layoutInfo.pPushConstantRanges      = &pushRange;
        // Guarda de idempotencia: recreateMsaaDependentPipelines vuelve a entrar
        // aqui para rehacer los cuatro pipelines con otro numero de muestras, y
        // el layout no depende de eso.
        if(m_pipelineLayout == VK_NULL_HANDLE &&
           vkCreatePipelineLayout(m_gpu.device(), &layoutInfo, nullptr, &m_pipelineLayout) != VK_SUCCESS)
        {
            throw std::runtime_error("failed to create pipeline layout!");
        }

        // 10. Pipeline completo
        VkGraphicsPipelineCreateInfo pipelineInfo{};
        pipelineInfo.sType                  = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        pipelineInfo.stageCount             = 2;
        pipelineInfo.pStages                = stages;
        pipelineInfo.pVertexInputState      = &vertexInput;
        pipelineInfo.pInputAssemblyState    = &inputAssembly;
        pipelineInfo.pViewportState         = &viewportInfo;
        pipelineInfo.pRasterizationState    = &rasterizationInfo;
        pipelineInfo.pMultisampleState      = &multisampleInfo;
        pipelineInfo.pColorBlendState       = &colorBlendInfo;
        pipelineInfo.pDynamicState          = &dynamicInfo;
        pipelineInfo.layout                 = m_pipelineLayout;
        pipelineInfo.renderPass             = m_offscreenRenderPass;
        pipelineInfo.subpass                = 0;
        pipelineInfo.pDepthStencilState     = &depthStencil;

        if(vkCreateGraphicsPipelines(m_gpu.device(), VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_pipeline) != VK_SUCCESS)
        {
            throw std::runtime_error("failed to create graphics pipeline!");
        }

        // Pipeline wireframe: mismo vertex input/layout/render pass, solo
        // cambia polygonMode a LINE y el fragment shader a color plano.
        auto wireFragCode = loadShaderFile("shaders/wireframe.frag.spv");
        VkShaderModule wireFragModule = createShaderModule(wireFragCode);

        VkPipelineShaderStageCreateInfo wireFragStage{};
        wireFragStage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        wireFragStage.stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
        wireFragStage.module = wireFragModule;
        wireFragStage.pName  = "main";

        VkPipelineShaderStageCreateInfo wireStages[] = { vertStage, wireFragStage };

        VkPipelineRasterizationStateCreateInfo wireRasterizationInfo = rasterizationInfo;
        wireRasterizationInfo.polygonMode = VK_POLYGON_MODE_LINE;

        VkGraphicsPipelineCreateInfo wirePipelineInfo = pipelineInfo;
        wirePipelineInfo.pStages             = wireStages;
        wirePipelineInfo.pRasterizationState = &wireRasterizationInfo;

        if(vkCreateGraphicsPipelines(m_gpu.device(), VK_NULL_HANDLE, 1, &wirePipelineInfo, nullptr, &m_wireframePipeline) != VK_SUCCESS)
        {
            throw std::runtime_error("failed to create wireframe graphics pipeline!");
        }

        // Pipeline del contorno de selección: mismo vertex input, mismo layout y
        // mismo render pass que el de arriba; solo cambian los dos shaders y el
        // culling, que pasa a descartar las caras FRONTALES. Las traseras del
        // casco extruido quedan por detrás de la superficie del objeto, así que
        // el depth test (LESS) solo deja pasar el reborde que sobresale de su
        // silueta. depthWrite se queda en TRUE a propósito: ese reborde tiene que
        // escribir profundidad o el skybox, que dibuja con LEQUAL donde nada
        // escribió, lo taparía.
        auto outlineVertCode = loadShaderFile("shaders/outline.vert.spv");
        auto outlineFragCode = loadShaderFile("shaders/outline.frag.spv");
        VkShaderModule outlineVertModule = createShaderModule(outlineVertCode);
        VkShaderModule outlineFragModule = createShaderModule(outlineFragCode);

        VkPipelineShaderStageCreateInfo outlineStages[2]{};
        outlineStages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        outlineStages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
        outlineStages[0].module = outlineVertModule;
        outlineStages[0].pName  = "main";
        outlineStages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        outlineStages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
        outlineStages[1].module = outlineFragModule;
        outlineStages[1].pName  = "main";

        VkPipelineRasterizationStateCreateInfo outlineRasterizationInfo = rasterizationInfo;
        outlineRasterizationInfo.cullMode = VK_CULL_MODE_FRONT_BIT;

        // Vertex input propio: outline.vert solo lee posición y normal, y
        // declarar los otros tres atributos haría saltar el warning
        // "Vertex attribute at location N not consumed by vertex shader" de la
        // capa de validación. Mismo binding y mismos offsets que arriba — solo
        // se describen menos atributos, el buffer que se bindea es el mismo.
        VkVertexInputAttributeDescription outlineAttrs[2]{};
        outlineAttrs[0] = { 0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, pos) };
        outlineAttrs[1] = { 3, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, normal) };

        VkPipelineVertexInputStateCreateInfo outlineVertexInput = vertexInput;
        outlineVertexInput.vertexAttributeDescriptionCount = 2;
        outlineVertexInput.pVertexAttributeDescriptions    = outlineAttrs;

        VkGraphicsPipelineCreateInfo outlinePipelineInfo = pipelineInfo;
        outlinePipelineInfo.pStages             = outlineStages;
        outlinePipelineInfo.pRasterizationState = &outlineRasterizationInfo;
        outlinePipelineInfo.pVertexInputState   = &outlineVertexInput;
        // El contorno se dibuja en el pass de composicion (ya en LDR) y no en el
        // de escena: si fuera por el pass HDR, el tonemap le cambiaria el naranja
        // plano. Alli el skybox ya esta dibujado, asi que el depthWrite del casco
        // deja de hacer falta para taparlo — se queda en TRUE igualmente porque
        // el pipeline hereda el depthStencil de arriba y no molesta a nadie.
        outlinePipelineInfo.renderPass          = m_compositeRenderPass;

        if(vkCreateGraphicsPipelines(m_gpu.device(), VK_NULL_HANDLE, 1, &outlinePipelineInfo, nullptr, &m_outlinePipeline) != VK_SUCCESS)
        {
            throw std::runtime_error("failed to create outline graphics pipeline!");
        }

        VkPipelineRasterizationStateCreateInfo outlineWireRasterizationInfo = outlineRasterizationInfo;
        outlineWireRasterizationInfo.polygonMode = VK_POLYGON_MODE_LINE;

        VkGraphicsPipelineCreateInfo outlineWirePipelineInfo = outlinePipelineInfo;
        outlineWirePipelineInfo.pRasterizationState = &outlineWireRasterizationInfo;

        if(vkCreateGraphicsPipelines(m_gpu.device(), VK_NULL_HANDLE, 1, &outlineWirePipelineInfo, nullptr, &m_outlineWirePipeline) != VK_SUCCESS)
        {
            throw std::runtime_error("failed to create outline wireframe graphics pipeline!");
        }

        // los módulos se destruyen al final de esta función — solo los necesita el pipeline
        vkDestroyShaderModule(m_gpu.device(), vertModule, nullptr);
        vkDestroyShaderModule(m_gpu.device(), fragModule, nullptr);
        vkDestroyShaderModule(m_gpu.device(), wireFragModule, nullptr);
        vkDestroyShaderModule(m_gpu.device(), outlineVertModule, nullptr);
        vkDestroyShaderModule(m_gpu.device(), outlineFragModule, nullptr);
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

    void Renderer::recreateSwapChain(Window& window)
    {
        int width = 0;
        int height = 0;

        glfwGetFramebufferSize(window.getNativeWindow(), &width, &height);
        // Espera si la ventana está minimizada (0x0)
        while(width == 0 || height == 0)
        {
            glfwWaitEvents();
            glfwGetFramebufferSize(window.getNativeWindow(), &width, &height);
        }

        vkDeviceWaitIdle(m_gpu.device());

        // Teardown offscreen primero (sus FBs usan m_depthImageView)
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
        createOffscreenImages(); // recrea con el nuevo tamaño

        // Solo recrear los semáforos que dependen del image count
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

        // Con batch, la copia se ejecuta al enviar la fence: destruir el staging
        // ahora sería un use-after-free en la GPU. Lo posee el batch hasta que
        // señala. Sin batch, la copia ya esperó (vkQueueWaitIdle) y se libera ya.
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

        // Ver createVertexBuffer: el staging vive hasta la fence cuando hay batch.
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

        // IBL: irradiancia (difuso) y entorno prefiltrado (especular). Los
        // consume solo pbr.frag, pero el layout lo comparten los tres pipelines
        // via m_pipelineLayout, asi que van aqui igual.
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

        // SSAO. Mismo criterio que los dos de arriba: solo lo lee pbr.frag, pero
        // el layout lo comparten los pipelines estatico, wireframe, skinned y
        // outline via m_pipelineLayout.
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
        for (int i = 0; i < MAX_FRAMES; i++) 
        {
            VkBufferCreateInfo bufferInfo{};
            bufferInfo.sType        = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            bufferInfo.size         = size;
            bufferInfo.usage        = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
            bufferInfo.sharingMode  = VK_SHARING_MODE_EXCLUSIVE;
            // Los cuatro VkResult se comprueban: el ultimo deja un puntero en
            // m_uniformBuffersMapped que updateUniformBuffer usa con memcpy en
            // CADA frame. Ignorarlos convertia un fallo de memoria en una
            // escritura sobre un puntero sin inicializar, que es un crash sin
            // relacion aparente con la causa.
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

            // Mapeo persistente — nunca llamamos unmap
            if (vkMapMemory(m_gpu.device(), m_uniformBuffersMemory[i], 0, size, 0, &m_uniformBuffersMapped[i]) != VK_SUCCESS)
                throw std::runtime_error("failed to map uniform buffer!");

        }
    }

    // ── Instancing: SSBO de transforms ──────────────────────────────────────
    // Capacidad inicial. No es un límite: ensureInstanceCapacity crece si la
    // escena tiene más objetos. 1024 matrices = 64 KB por frame, lo que cubre
    // sin realojar cualquier escena normal.
    static constexpr uint32_t kInstanceInitialCapacity = 1024;

    void Renderer::destroyInstanceBuffer(int frame)
    {
        if (m_instanceBuffers[frame] == VK_NULL_HANDLE) return;
        // El mapeo persistente muere con la memoria; no hace falta unmap
        // explícito, pero sí olvidar el puntero para no escribir en él si algo
        // fallara entre el destroy y el create.
        m_instanceMapped[frame] = nullptr;
        vkDestroyBuffer(m_gpu.device(), m_instanceBuffers[frame], nullptr);
        vkFreeMemory(m_gpu.device(), m_instanceMemory[frame], nullptr);
        m_instanceBuffers[frame]  = VK_NULL_HANDLE;
        m_instanceMemory[frame]   = VK_NULL_HANDLE;
        m_instanceCapacity[frame] = 0;
    }

    void Renderer::createInstanceResources()
    {
        // Set 1: un solo storage buffer, solo lo lee el vertex shader
        // (triangle.vert y shadow.vert).
        VkDescriptorSetLayoutBinding binding{};
        binding.binding         = 0;
        binding.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        binding.descriptorCount = 1;
        binding.stageFlags      = VK_SHADER_STAGE_VERTEX_BIT;

        VkDescriptorSetLayoutCreateInfo dslInfo{};
        dslInfo.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        dslInfo.bindingCount = 1;
        dslInfo.pBindings    = &binding;
        if (vkCreateDescriptorSetLayout(m_gpu.device(), &dslInfo, nullptr, &m_instanceDescLayout) != VK_SUCCESS)
            throw std::runtime_error("failed to create instance descriptor set layout!");

        // Pool propio y no la cadena m_descriptorPools: esa se reparte por objeto y solo
        // tiene UNIFORM_BUFFER y COMBINED_IMAGE_SAMPLER. Aquí hacen falta
        // exactamente MAX_FRAMES sets con un STORAGE_BUFFER cada uno, y los sets
        // viven todo el proceso (no se liberan nunca).
        VkDescriptorPoolSize poolSize{};
        poolSize.type            = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        poolSize.descriptorCount = MAX_FRAMES;

        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.poolSizeCount = 1;
        poolInfo.pPoolSizes    = &poolSize;
        poolInfo.maxSets       = MAX_FRAMES;
        if (vkCreateDescriptorPool(m_gpu.device(), &poolInfo, nullptr, &m_instanceDescPool) != VK_SUCCESS)
            throw std::runtime_error("failed to create instance descriptor pool!");

        VkDescriptorSetLayout layouts[MAX_FRAMES] = { m_instanceDescLayout, m_instanceDescLayout };
        VkDescriptorSetAllocateInfo allocInfo{};
        allocInfo.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocInfo.descriptorPool     = m_instanceDescPool;
        allocInfo.descriptorSetCount = MAX_FRAMES;
        allocInfo.pSetLayouts        = layouts;
        if (vkAllocateDescriptorSets(m_gpu.device(), &allocInfo, m_instanceDescSets) != VK_SUCCESS)
            throw std::runtime_error("failed to allocate instance descriptor sets!");

        // Los buffers de los DOS frames, no solo el actual: el descriptor set de
        // cada frame tiene que apuntar a algo válido desde el primer draw.
        const int saved = m_currentFrame;
        for (int i = 0; i < MAX_FRAMES; i++)
        {
            m_currentFrame = i;
            ensureInstanceCapacity(kInstanceInitialCapacity);
        }
        m_currentFrame = saved;
    }

    void Renderer::ensureInstanceCapacity(uint32_t matrices)
    {
        const int frame = m_currentFrame;
        if (matrices <= m_instanceCapacity[frame]) return;

        // Duplicar en vez de ajustar al pelo: instanciar un objeto por frame
        // (scripts Lua) no debe recrear el buffer en cada uno.
        uint32_t capacity = m_instanceCapacity[frame] ? m_instanceCapacity[frame] : kInstanceInitialCapacity;
        while (capacity < matrices) capacity *= 2;

        destroyInstanceBuffer(frame);

        VkDeviceSize size = (VkDeviceSize)capacity * sizeof(glm::mat4);

        VkBufferCreateInfo bufferInfo{};
        bufferInfo.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size        = size;
        bufferInfo.usage       = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(m_gpu.device(), &bufferInfo, nullptr, &m_instanceBuffers[frame]) != VK_SUCCESS)
            throw std::runtime_error("failed to create instance buffer!");

        VkMemoryRequirements memReq;
        vkGetBufferMemoryRequirements(m_gpu.device(), m_instanceBuffers[frame], &memReq);

        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize  = memReq.size;
        allocInfo.memoryTypeIndex = m_gpu.findMemoryType(memReq.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (vkAllocateMemory(m_gpu.device(), &allocInfo, nullptr, &m_instanceMemory[frame]) != VK_SUCCESS)
            throw std::runtime_error("failed to allocate instance buffer memory!");
        vkBindBufferMemory(m_gpu.device(), m_instanceBuffers[frame], m_instanceMemory[frame], 0);

        // Mapeo persistente, como los uniform buffers: se escribe cada frame.
        vkMapMemory(m_gpu.device(), m_instanceMemory[frame], 0, size, 0, &m_instanceMapped[frame]);
        m_instanceCapacity[frame] = capacity;

        VkDescriptorBufferInfo dbi{};
        dbi.buffer = m_instanceBuffers[frame];
        dbi.offset = 0;
        dbi.range  = size;

        VkWriteDescriptorSet write{};
        write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet          = m_instanceDescSets[frame];
        write.dstBinding      = 0;
        write.descriptorCount = 1;
        write.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        write.pBufferInfo     = &dbi;
        vkUpdateDescriptorSets(m_gpu.device(), 1, &write, 0, nullptr);
    }

    uint32_t Renderer::buildInstanceBatches(const BatchCandidate* candidates,
                                            size_t                count,
                                            glm::mat4*            outTransforms,
                                            uint32_t              outCapacity,
                                            uint32_t              firstInstanceBase,
                                            std::vector<InstanceBatch>& outBatches)
    {
        // El agrupado vive en Renderer/InstanceBatching.{h,cpp} desde que hay un
        // segundo backend que lo necesita: no tiene una linea de Vulkan, y
        // arrastrar vulkan.h al de DirectX 12 por esto no tiene sentido. Este
        // delegado se queda para no tocar a los llamantes ni a instancing_tests.
        return Batching::buildInstanceBatches(candidates, count, outTransforms, outCapacity,
                                              firstInstanceBase, outBatches);
    }

    void Renderer::createDescriptorPool()
    {
        // El primero de la cadena. Antes esto era el UNICO, dimensionado con las
        // mallas que hubiera en el arranque mas 128 de margen; pasado el margen,
        // vkAllocateDescriptorSets fallaba y se lanzaba en mitad de un Load
        // Scene. Ahora el tamano es fijo y allocateSharedSets encadena otro
        // cuando hace falta.
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
        // FREE_DESCRIPTOR_SET: rebuildSkinnedMesh destruye y recrea el objeto en
        // su sitio, y sin poder devolver los sets al pool cada reimportación
        // consumiría slots hasta agotarlo.
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

        // Dos intentos: el ultimo pool y, si esta lleno, uno recien creado. Los
        // anteriores no se recorren: al liberar, los huecos vuelven a SU pool y
        // podrian reaprovecharse, pero buscarlos costaria una llamada fallida
        // por pool en el camino normal. Lo que se pierde es memoria, no
        // correccion.
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
        // Por entrada compartida, no por objeto: initSceneResources construye
        // primero todos los RenderObject (sin pool todavía) y luego llama aquí.
        // Iterar m_objects alojaría N sets para la misma entrada y los N-1
        // primeros se perderían.
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

            // IBL: los mismos dos cubemaps para todos los objetos. Existen desde
            // init(), asi que estos writes valen aunque no haya skybox.
            VkDescriptorImageInfo irradianceInfo{};
            irradianceInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            irradianceInfo.imageView   = m_iblPass.irradianceView();
            irradianceInfo.sampler     = m_iblPass.sampler();

            VkDescriptorImageInfo prefilterInfo{};
            prefilterInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            prefilterInfo.imageView   = m_iblPass.prefilterView();
            prefilterInfo.sampler     = m_iblPass.sampler();

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

            writes[5].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; writes[5].dstSet = obj.descriptorSets[i];
            writes[5].dstBinding = 5; writes[5].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[5].descriptorCount = 1; writes[5].pImageInfo = &irradianceInfo;

            writes[6].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; writes[6].dstSet = obj.descriptorSets[i];
            writes[6].dstBinding = 6; writes[6].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[6].descriptorCount = 1; writes[6].pImageInfo = &prefilterInfo;

            vkUpdateDescriptorSets(m_gpu.device(), 7, writes, 0, nullptr);

            // Binding 7 (SSAO) aparte: es la única vista de este set que se
            // destruye y se rehace al redimensionar, y refreshSsaoDescriptors
            // vuelve a pasar por aquí con los mismos handles.
            writeSsaoBinding(obj.descriptorSets[i], i);
        }
    }

    void Renderer::writeSsaoBinding(VkDescriptorSet set, int frameIndex)
    {
        // Sin imagen todavía (init temprano) no hay nada válido que escribir: el
        // set se completa desde refreshSsaoDescriptors en cuanto exista.
        if (m_ssaoPass.blurViews()[frameIndex] == VK_NULL_HANDLE) return;

        VkDescriptorImageInfo info{};
        // GENERAL y no SHADER_READ_ONLY: la misma imagen es storage image del
        // compute y textura del pass de escena, igual que la cadena del bloom.
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
        // Los skinned tienen sus propios sets del mismo layout, uno por material.
        for (const SkinnedRenderObject& sobj : m_skinnedObjects)
            for (const SkinnedMatGfx& mgfx : sobj.matGfx)
                for (int i = 0; i < MAX_FRAMES; i++)
                    if (mgfx.descSets[i]) writeSsaoBinding(mgfx.descSets[i], i);
    }

    void Renderer::setSsaoEnabled(bool v)
    {
        if (v == ssaoEnabled()) return;
        setSsaoEnabledFlag(v);
        // Al apagar, el mapa se queda con el AO del último frame calculado y
        // seguiría oscureciendo. Un clear a 1.0 por frame en vuelo lo devuelve a
        // la identidad; a partir de ahí, cero trabajo.
        if (!v)
            m_ssaoPass.markClearPending();
    }

    // Secuencia de Halton en base b: la sucesión de baja discrepancia con la que
    // el TAA reparte las muestras dentro del píxel. Cubre el área mucho más
    // uniformemente que un aleatorio, que es lo que hace que el promedio temporal
    // converja a un supersampling de verdad.
    static float halton(uint32_t index, uint32_t base)
    {
        float result = 0.0f;
        float f      = 1.0f;
        while (index > 0)
        {
            f      /= (float)base;
            result += f * (float)(index % base);
            index  /= base;
        }
        return result;
    }

    void Renderer::updateUniformBuffer(uint32_t frameIndex)
    {
        // Cámara del CameraComponent en Play, la del editor en edición. El
        // Y-flip de Vulkan ya viene aplicado desde currentFrameCamera().
        const FrameCamera fc = currentFrameCamera();

        // View-proj SIN jitter: es la que reproyecta el TAA y la que se compara
        // con la del frame anterior. El jitter es ruido de muestreo, no
        // movimiento de cámara, y meterlo aquí arrastraría el historial.
        //
        // El relevo prev←curr se hace aquí y TODOS los frames porque el motion
        // blur también reproyecta con estas dos, y corre con el TAA apagado. La
        // línea equivalente del final del pass del TAA se queda donde está y
        // escribe exactamente el mismo valor: con el TAA activo esto es
        // redundante, no un cambio.
        m_aaPass.updateFrameMatrices(aaCtx(), fc.view, fc.proj);

        UniformBufferObject ubo{};
        ubo.view = fc.view;
        // Con TAA sale jittereada; en cualquier otro modo es fc.proj tal cual.
        ubo.proj = m_aaPass.jitteredProj();
        ubo.numLights        = std::min((int)m_lights.size(), MAX_LIGHTS);
        ubo.ambientIntensity = m_ambientEnabled ? m_ambientIntensity : 0.0f;
        for(int i = 0; i < ubo.numLights; i++)
        {
            ubo.lights[i] = m_lights[i];
        }
        
        ubo.viewPos  = glm::vec4(fc.eye, 1.0f);
        // Las cascadas ya las calculó draw() para este frame. Copiarlas y no
        // recalcularlas es lo que garantiza que el fragment shader muestree con
        // exactamente las mismas matrices con las que se culeó y se grabó el
        // pass de sombras.
        for (int i = 0; i < SHADOW_CASCADES; i++)
        {
            ubo.lightSpaceMatrix[i] = m_shadowPass.cascadeMatrix(i);
        }
        ubo.cascadeSplits = m_shadowPass.cascadeSplits();

        memcpy(m_uniformBuffersMapped[frameIndex], &ubo, sizeof(ubo));
        // El bake de una reflection probe parte de este mismo buffer (luces y
        // matrices de cascada del frame); hasta que se escribe una vez es basura.
        m_uboWritten[frameIndex] = true;

        // ── Forward+: bloque de parametros y lista de luces ──────────────────
        // El frame que toca no es m_currentFrame sino frameIndex (el bakeo de
        // sondas escribe el 0), asi que el contexto se arma con ese.
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
        // Tamano y muestras INTERNOS: este depth lo comparten el pass de escena y
        // el de composicion, asi que sigue al SSAA (mas resolucion) y al MSAA
        // (mas muestras). El del pre-pass del SSAO/SSR es otro y siempre va a una.
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
        return created;
    }

    void Renderer::createSharedGpuMesh(const Mesh& mesh, SharedGpuMesh& obj,
                                       TransferBatch* batch,
                                       const std::vector<DecodedImage>* decoded)
    {
        obj.indexCount = (uint32_t)mesh.indices.size();

        // AABB local para el frustum culling. Se calcula aquí y no en el bucle
        // de dibujo porque la geometría no cambia después: lo único que se
        // mueve es el transform del RenderObject, y el test ya la transforma
        // cada frame.
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

        // Busca un slot entre los píxeles que ya decodificó el worker. Sin
        // acierto se cae a la ruta de siempre (stbi_load en este hilo), que es
        // lo que pasa en el init síncrono y en las texturas sin decodificar.
        auto findSlot = [decoded](DecodedImage::Slot s) -> const DecodedImage* {
            if (!decoded) return nullptr;
            for (const auto& d : *decoded) if (d.slot == s) return &d;
            return nullptr;
        };

        if (const DecodedImage* albedo = findSlot(DecodedImage::Albedo))
            m_res.createTextureImageFromPixels(albedo->pixels.data(),
                                               (uint32_t)albedo->w, (uint32_t)albedo->h,
                                               obj.textureImage, obj.textureMem, batch);
        else
            m_res.createTextureImage(mesh.material.texturePath, mesh.material.embeddedTexture,
                                     obj.textureImage, obj.textureMem, batch);
        m_res.createTextureImageView(obj.textureImage, obj.textureView);
        m_res.createTextureSampler(obj.sampler);

        if (const DecodedImage* normal = findSlot(DecodedImage::Normal))
            m_res.createNormalMapImageFromPixels(normal->pixels.data(),
                                                 (uint32_t)normal->w, (uint32_t)normal->h,
                                                 obj.normalImage, obj.normalMem, batch);
        else
            m_res.createNormalMapImage(mesh.material.normalMapPath, mesh.material.embeddedNormalMap,
                                       obj.normalImage, obj.normalMem, batch);
        m_res.createTextureImageView(obj.normalImage, obj.normalView, VK_FORMAT_R8G8B8A8_UNORM);
        m_res.createTextureSampler(obj.normalSampler);

        if (const DecodedImage* orm = findSlot(DecodedImage::ORM))
        {
            m_res.createNormalMapImageFromPixels(orm->pixels.data(),
                                                 (uint32_t)orm->w, (uint32_t)orm->h,
                                                 obj.ormImage, obj.ormMem, batch);
            obj.metallic  = 1.0f;
            obj.roughness = 1.0f;
        }
        else if (!mesh.material.metallicRoughnessPath.empty()
                 || !mesh.material.embeddedMetallicRoughness.empty())
        {
            m_res.createNormalMapImage(mesh.material.metallicRoughnessPath,
                                       mesh.material.embeddedMetallicRoughness,
                                       obj.ormImage, obj.ormMem, batch);
            obj.metallic  = 1.0f;
            obj.roughness = 1.0f;
        }
        else
        {
            constexpr uint8_t white[4] = {255, 255, 255, 255};
            m_res.createSolidColorImage(white, obj.ormImage, obj.ormMem, batch);
            obj.metallic  = mesh.material.metallic;
            obj.roughness = mesh.material.roughness;
        }
        m_res.createTextureImageView(obj.ormImage, obj.ormView, VK_FORMAT_R8G8B8A8_UNORM);
        m_res.createTextureSampler(obj.ormSampler);
    }

    int Renderer::addStaticMesh(const Mesh& mesh, const std::vector<DecodedImage>* decoded)
    {
        if (!m_pendingBatch)
            m_pendingBatch = std::make_unique<TransferBatch>(m_gpu);

        m_objects.emplace_back();
        RenderObject& obj = m_objects.back();
        const bool created = buildRenderObject(mesh, obj, m_pendingBatch.get(), decoded);

        if (created)
        {
            SharedGpuMesh& gpu = *m_sharedMeshes.get(obj.sharedIndex);
            allocateObjectDescriptorSet(gpu);
            // La entrada no se dibuja hasta que la fence de este batch señale.
            // Es la decisión de producto de la spec: nada de placeholders, el
            // GameObject aparece cuando está listo. Si la entrada ya existía no
            // se toca su ticket: o ya está subida (0), o sigue esperando el
            // suyo, y machacarlo con uno posterior la retrasaría de más.
            gpu.uploadTicket = m_nextUploadTicket;
        }
        return (int)m_objects.size() - 1;
    }

    void Renderer::destroySharedGpuMesh(const SharedGpuMesh& obj)
    {
        vkDestroySampler(m_gpu.device(),   obj.ormSampler,    nullptr);
        vkDestroyImageView(m_gpu.device(), obj.ormView,       nullptr);
        vkDestroyImage(m_gpu.device(),     obj.ormImage,      nullptr);
        vkFreeMemory(m_gpu.device(),       obj.ormMem,        nullptr);
        vkDestroySampler(m_gpu.device(),   obj.normalSampler, nullptr);
        vkDestroyImageView(m_gpu.device(), obj.normalView,    nullptr);
        vkDestroyImage(m_gpu.device(),     obj.normalImage,   nullptr);
        vkFreeMemory(m_gpu.device(),       obj.normalMem,     nullptr);
        vkDestroySampler(m_gpu.device(),   obj.sampler,       nullptr);
        vkDestroyImageView(m_gpu.device(), obj.textureView,   nullptr);
        vkDestroyImage(m_gpu.device(),     obj.textureImage,  nullptr);
        vkFreeMemory(m_gpu.device(),       obj.textureMem,    nullptr);
        vkDestroyBuffer(m_gpu.device(),    obj.indexBuffer,   nullptr);
        vkFreeMemory(m_gpu.device(),       obj.indexMemory,   nullptr);
        vkDestroyBuffer(m_gpu.device(),    obj.vertexBuffer,  nullptr);
        vkFreeMemory(m_gpu.device(),       obj.vertexMemory,  nullptr);

        // Los sets vuelven al pool (creado con FREE_DESCRIPTOR_SET_BIT), igual
        // que destroySkinnedRenderObject: sin esto, cada removeStaticObject /
        // rebuild agotaba su pool en vez de reciclar sus sets. No hace
        // falta anularlos: obj es siempre la copia que la cache sacó de la
        // tabla, y el slot original ya quedó vacío.
        if (obj.descriptorSets[0] != VK_NULL_HANDLE && obj.descPool != VK_NULL_HANDLE)
            vkFreeDescriptorSets(m_gpu.device(), obj.descPool, MAX_FRAMES, obj.descriptorSets);
    }

    void Renderer::recordShadowPass(VkCommandBuffer cmd)
    {
        // Sin luces no hay matrices que extraer (computeCascades deja la
        // identidad, cuyo frustum es el cubo unidad y culearía casi todo). Aun
        // así hay que abrir los N render pass: son los que limpian las capas y
        // las dejan en DEPTH_STENCIL_READ_ONLY_OPTIMAL, que es el layout que
        // declaran los descriptor sets. Lo que se salta es la geometría, que
        // nadie va a muestrear (numLights = 0 apaga el shadow en el shader).
        const bool drawCasters = !m_lights.empty();

        for (uint32_t cascade = 0; cascade < SHADOW_CASCADES; cascade++)
        {
            // Render pass, viewport, scissor, pipeline y push del índice: del
            // pase. Los draws de aquí abajo son del Renderer.
            m_shadowPass.beginCascade(cmd, cascade);

            if (!drawCasters)
            {
                m_shadowPass.endCascade(cmd);
                continue;
            }

            // Culling por el frustum de ESTA cascada, no por el de la cámara ni
            // por el de la cascada mayor: un objeto que la cámara no ve puede
            // seguir proyectando sombra sobre lo que sí se ve, y un objeto que
            // cae en la cascada lejana no pinta nada en el mapa de la cercana.
            const Frustum lightFrustum = frustumFromViewProj(m_shadowPass.cascadeMatrix(cascade));

            // Mismas guardas por objeto que el pass principal, con el frustum de
            // la luz. El agrupado es independiente del de la cámara: los
            // conjuntos visibles no coinciden, así que cada pass escribe su
            // propio rango del SSBO (las cascadas van primero, una detrás de
            // otra, y el de la escena al final).
            m_batchCandidates.clear();
            m_batchCandidates.reserve(m_objects.size());
            for(auto& obj : m_objects)
            {
                const SharedGpuMesh* gpu = m_sharedMeshes.get(obj.sharedIndex);
                // !gpu: borrado desde el editor. En vuelo: no debe proyectar
                // sombra si todavía no es visible, o habría una sombra flotando
                // sin objeto que la eche. obj.meshVisible (checkbox "Visible"):
                // un mesh oculto no se manda a la GPU en ningún pass, así que
                // tampoco proyecta sombra.
                bool visible = obj.meshVisible && gpu && gpu->uploadTicket <= m_lastCompletedTicket;
                // Fuera del volumen que cubre esta cascada: su sombra no cabría
                // en la capa de todos modos.
                if (visible && gpu->hasBounds &&
                    !aabbVisible(lightFrustum, gpu->aabbMin, gpu->aabbMax, obj.transform))
                {
                    visible = false;
                }
                m_batchCandidates.push_back({ obj.sharedIndex, visible, &obj.transform });
            }

            const uint32_t instanceBase = m_instanceCursor;
            glm::mat4* dst = (glm::mat4*)m_instanceMapped[m_currentFrame] + instanceBase;
            m_instanceCursor += buildInstanceBatches(m_batchCandidates.data(), m_batchCandidates.size(),
                dst, m_instanceCapacity[m_currentFrame] - instanceBase, instanceBase, m_instanceBatches);

            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_shadowPass.pipelineLayout(),
                1, 1, &m_instanceDescSets[m_currentFrame], 0, nullptr);

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

            // Skinned. SkinningPass::record corre justo antes en este mismo command
            // buffer y deja outputVertexBuffer con la pose de ESTE frame y una
            // barrera compute → VERTEX_INPUT, así que aquí ya se puede leer como
            // vertex buffer. Mismo shader, mismo layout, mismo SSBO de
            // instancias y misma matriz de cascada que los estáticos: lo único
            // propio es el pipeline con el stride de SkinnedVertex.
            bool skinnedBound = false;
            for (size_t si = 0; si < m_skinnedObjects.size(); si++)
            {
                // Misma lista de visibles que consumió el compute: a un objeto
                // al que no se le despachó skinning le quedaría la pose del
                // último frame en que fue visible, así que su sombra sería
                // falsa. Se paga que un personaje fuera de cámara no proyecte.
                if (si >= m_skinnedVisible.size() || !m_skinnedVisible[si]) continue;
                const SkinnedRenderObject& sobj = m_skinnedObjects[si];
                // Checkbox "Visible" del componente Mesh: oculto no proyecta.
                if (!sobj.meshVisible) continue;
                if (sobj.outputVertexBuffer == VK_NULL_HANDLE || sobj.matGfx.empty()) continue;
                // Sin sitio en el SSBO de instancias de este frame: mejor sin
                // sombra que pisar el rango de otro pass.
                if (m_instanceCursor >= m_instanceCapacity[m_currentFrame]) break;

                // shadow.vert saca el model matrix del SSBO por gl_InstanceIndex:
                // una entrada por objeto y un draw de una instancia apuntando a
                // ella con firstInstance.
                const uint32_t instanceIndex = m_instanceCursor++;
                ((glm::mat4*)m_instanceMapped[m_currentFrame])[instanceIndex] = sobj.transform;

                if (!skinnedBound)
                {
                    // El layout es el mismo, así que el push constant de la
                    // cascada sobrevive al cambio de pipeline; el pase lo
                    // reescribe por no depender de esa compatibilidad.
                    m_shadowPass.bindSkinnedPipeline(cmd, cascade);
                    skinnedBound = true;
                }

                // Set 0 solo por el UBO de la cascada (binding 0): shadow.vert no
                // muestrea nada, así que cualquier descriptor set del material
                // sirve mientras sea del layout que declara el pipeline.
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

    // Los cuatro pipelines GRAFICOS de las mallas con huesos. Comparten
    // shaders con los estaticos y solo cambian el vertex input (stride 80,
    // la salida del compute de skinning). Se rehacen al cambiar el MSAA, que
    // es lo que los separa de los tres compute de SkinningPass.
    void Renderer::createSkinnedGraphicsPipelines()
    {
         // --- Skinned graphics pipeline (stride=80, mismos shaders) ---
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

            VkPipelineInputAssemblyStateCreateInfo ia{};
            ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
            ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

            VkPipelineViewportStateCreateInfo vp{};
            vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
            vp.viewportCount = 1; vp.scissorCount = 1;

            VkPipelineRasterizationStateCreateInfo rs{};
            rs.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
            rs.polygonMode = VK_POLYGON_MODE_FILL;
            rs.cullMode    = VK_CULL_MODE_BACK_BIT;
            rs.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
            rs.lineWidth   = 1.0f;

            VkPipelineMultisampleStateCreateInfo ms{};
            ms.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
            // Como los estaticos: estos cuatro viven en el pass de escena y en el
            // de composicion, que siguen el numero de muestras del modo de AA.
            ms.rasterizationSamples = m_aaSampleCount;

            VkPipelineDepthStencilStateCreateInfo ds{};
            ds.sType            = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
            ds.depthTestEnable  = VK_TRUE;
            ds.depthWriteEnable = VK_TRUE;
            ds.depthCompareOp   = VK_COMPARE_OP_LESS;

            VkPipelineColorBlendAttachmentState blend{};
            blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

            VkPipelineColorBlendStateCreateInfo cb{};
            cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
            cb.attachmentCount = 1; cb.pAttachments = &blend;

            VkDynamicState dynStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
            VkPipelineDynamicStateCreateInfo dyn{};
            dyn.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
            dyn.dynamicStateCount = 2; dyn.pDynamicStates = dynStates;

            VkGraphicsPipelineCreateInfo pci{};
            pci.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
            pci.stageCount          = 2;  pci.pStages             = stages;
            pci.pVertexInputState   = &vi; pci.pInputAssemblyState = &ia;
            pci.pViewportState      = &vp; pci.pRasterizationState = &rs;
            pci.pMultisampleState   = &ms; pci.pDepthStencilState  = &ds;
            pci.pColorBlendState    = &cb; pci.pDynamicState       = &dyn;
            pci.layout              = m_pipelineLayout;
            pci.renderPass          = m_offscreenRenderPass;
            pci.subpass             = 0;

            if (vkCreateGraphicsPipelines(m_gpu.device(), VK_NULL_HANDLE, 1, &pci, nullptr, &m_skinnedGfxPipeline) != VK_SUCCESS)
                throw std::runtime_error("failed to create skinned graphics pipeline!");

            // Pipeline wireframe skinned: mismo vertex input/layout que el
            // gfx pipeline de arriba, solo cambia polygonMode a LINE y el
            // fragment shader a color plano.
            auto wireFragCode = loadShaderFile("shaders/wireframe.frag.spv");
            auto wireFragMod  = createShaderModule(wireFragCode);

            VkPipelineShaderStageCreateInfo wireFragStage{};
            wireFragStage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            wireFragStage.stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
            wireFragStage.module = wireFragMod;
            wireFragStage.pName  = "main";

            VkPipelineShaderStageCreateInfo wireStages[] = { stages[0], wireFragStage };

            VkPipelineRasterizationStateCreateInfo wireRs = rs;
            wireRs.polygonMode = VK_POLYGON_MODE_LINE;

            VkGraphicsPipelineCreateInfo wirePci = pci;
            wirePci.pStages             = wireStages;
            wirePci.pRasterizationState = &wireRs;

            if (vkCreateGraphicsPipelines(m_gpu.device(), VK_NULL_HANDLE, 1, &wirePci, nullptr, &m_skinnedWireframePipeline) != VK_SUCCESS)
                throw std::runtime_error("failed to create skinned wireframe pipeline!");

            // Contorno de selección skinned: gemelo del estático de
            // createPipeline (mismos shaders, cullMode FRONT), pero sobre este
            // vertex input de stride 80. El buffer que lee es el de SALIDA del
            // compute de skinning, así que el casco se extruye sobre la pose ya
            // deformada de este frame, no sobre la de reposo.
            auto outlineVertCode = loadShaderFile("shaders/outline.vert.spv");
            auto outlineFragCode = loadShaderFile("shaders/outline.frag.spv");
            auto outlineVertMod  = createShaderModule(outlineVertCode);
            auto outlineFragMod  = createShaderModule(outlineFragCode);

            VkPipelineShaderStageCreateInfo outlineStages[2]{};
            outlineStages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            outlineStages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
            outlineStages[0].module = outlineVertMod; outlineStages[0].pName = "main";
            outlineStages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            outlineStages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
            outlineStages[1].module = outlineFragMod; outlineStages[1].pName = "main";

            VkPipelineRasterizationStateCreateInfo outlineRs = rs;
            outlineRs.cullMode = VK_CULL_MODE_FRONT_BIT;

            // Solo pos@0 y normal@48 de OutputVertex, por lo mismo que en
            // createPipeline: outline.vert no consume color, uv ni tangent.
            VkVertexInputAttributeDescription outlineAttrs[2]{};
            outlineAttrs[0] = { 0, 0, VK_FORMAT_R32G32B32_SFLOAT,  0 };
            outlineAttrs[1] = { 3, 0, VK_FORMAT_R32G32B32_SFLOAT, 48 };

            VkPipelineVertexInputStateCreateInfo outlineVi = vi;
            outlineVi.vertexAttributeDescriptionCount = 2;
            outlineVi.pVertexAttributeDescriptions    = outlineAttrs;

            VkGraphicsPipelineCreateInfo outlinePci = pci;
            outlinePci.pStages             = outlineStages;
            outlinePci.pRasterizationState = &outlineRs;
            outlinePci.pVertexInputState   = &outlineVi;
            // Pass de composicion, igual que el contorno estatico: fuera del
            // tonemap para que el naranja siga siendo el mismo naranja.
            outlinePci.renderPass          = m_compositeRenderPass;

            if (vkCreateGraphicsPipelines(m_gpu.device(), VK_NULL_HANDLE, 1, &outlinePci, nullptr, &m_skinnedOutlinePipeline) != VK_SUCCESS)
                throw std::runtime_error("failed to create skinned outline pipeline!");

            VkPipelineRasterizationStateCreateInfo outlineWireRs = outlineRs;
            outlineWireRs.polygonMode = VK_POLYGON_MODE_LINE;

            VkGraphicsPipelineCreateInfo outlineWirePci = outlinePci;
            outlineWirePci.pRasterizationState = &outlineWireRs;

            if (vkCreateGraphicsPipelines(m_gpu.device(), VK_NULL_HANDLE, 1, &outlineWirePci, nullptr, &m_skinnedOutlineWirePipeline) != VK_SUCCESS)
                throw std::runtime_error("failed to create skinned outline wireframe pipeline!");

            vkDestroyShaderModule(m_gpu.device(), vertMod, nullptr);
            vkDestroyShaderModule(m_gpu.device(), fragMod, nullptr);
            vkDestroyShaderModule(m_gpu.device(), wireFragMod, nullptr);
            vkDestroyShaderModule(m_gpu.device(), outlineVertMod, nullptr);
            vkDestroyShaderModule(m_gpu.device(), outlineFragMod, nullptr);
        }
    }

    void Renderer::destroySkinnedRenderObject(SkinnedRenderObject& obj)
    {
        auto destroy = [&](VkBuffer& b, VkDeviceMemory& m)
        {
            if (b != VK_NULL_HANDLE) { vkDestroyBuffer(m_gpu.device(), b, nullptr); b = VK_NULL_HANDLE; }
            if (m != VK_NULL_HANDLE) { vkFreeMemory(m_gpu.device(), m, nullptr);    m = VK_NULL_HANDLE; }
        };
        destroy(obj.keyframePosBuffer,    obj.keyframePosMemory);
        destroy(obj.keyframeRotBuffer,    obj.keyframeRotMemory);
        destroy(obj.keyframeScaleBuffer,  obj.keyframeScaleMemory);
        destroy(obj.boneInfoBuffer,       obj.boneInfoMemory);
        destroy(obj.inputVertexBuffer,    obj.inputVertexMemory);
        destroy(obj.localTransformBuffer, obj.localTransformMemory);
        destroy(obj.finalBoneBuffer,      obj.finalBoneMemory);
        destroy(obj.outputVertexBuffer,   obj.outputVertexMemory);
        destroy(obj.indexBuffer,          obj.indexMemory);

        for (auto& mgfx : obj.matGfx)
        {
            if (mgfx.ormSampler    != VK_NULL_HANDLE) { vkDestroySampler  (m_gpu.device(), mgfx.ormSampler,    nullptr); }
            if (mgfx.ormView       != VK_NULL_HANDLE) { vkDestroyImageView(m_gpu.device(), mgfx.ormView,       nullptr); }
            if (mgfx.ormImage      != VK_NULL_HANDLE) { vkDestroyImage    (m_gpu.device(), mgfx.ormImage,      nullptr); }
            if (mgfx.ormMem        != VK_NULL_HANDLE) { vkFreeMemory      (m_gpu.device(), mgfx.ormMem,        nullptr); }
            if (mgfx.normalSampler != VK_NULL_HANDLE) { vkDestroySampler  (m_gpu.device(), mgfx.normalSampler, nullptr); }
            if (mgfx.normalView    != VK_NULL_HANDLE) { vkDestroyImageView(m_gpu.device(), mgfx.normalView,    nullptr); }
            if (mgfx.normalImage   != VK_NULL_HANDLE) { vkDestroyImage    (m_gpu.device(), mgfx.normalImage,   nullptr); }
            if (mgfx.normalMem     != VK_NULL_HANDLE) { vkFreeMemory      (m_gpu.device(), mgfx.normalMem,     nullptr); }
            if (mgfx.sampler       != VK_NULL_HANDLE) { vkDestroySampler  (m_gpu.device(), mgfx.sampler,       nullptr); }
            if (mgfx.textureView   != VK_NULL_HANDLE) { vkDestroyImageView(m_gpu.device(), mgfx.textureView,   nullptr); }
            if (mgfx.textureImage  != VK_NULL_HANDLE) { vkDestroyImage    (m_gpu.device(), mgfx.textureImage,  nullptr); }
            if (mgfx.textureMem    != VK_NULL_HANDLE) { vkFreeMemory      (m_gpu.device(), mgfx.textureMem,    nullptr); }
        }

        // Los sets vuelven al pool (creado con FREE_DESCRIPTOR_SET_BIT): sin
        // esto, reconstruir el objeto lo agotaría.
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

        m_skinnedObjects.emplace_back();
        SkinnedRenderObject& obj = m_skinnedObjects.back();
        initSkinnedRenderObject(obj, mesh, m_pendingBatch.get(), decoded);

        // Igual que los estáticos: invisible hasta que la fence del batch señale.
        obj.uploadTicket = m_nextUploadTicket;
        return (int)m_skinnedObjects.size() - 1;
    }

    void Renderer::initSkinnedRenderObject(SkinnedRenderObject& obj, const SkinnedMesh& mesh,
                                           TransferBatch* batch,
                                           const std::vector<DecodedImage>* decoded)
    {
        // Las texturas de los materiales skinned no se decodifican en el worker
        // (no hay forma de mapear un DecodedImage a un submesh concreto), así que
        // toman siempre la ruta síncrona de stbi_load — pero SIEMPRE con batch,
        // para que sus uploads caigan en m_pendingBatch y el ticket se resuelva.
        (void)decoded;
        const Skeleton&      skel = mesh.skeleton;
        // Clip 0 pa duration/ticksPerSecond del objeto: son lo que consume
        // updateAnimation(), que solo corre en el caso SIN Animator (Task 3
        // añade el camino con Animator). Malla sin animaciones -> clip vacío.
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
        // Cota del culling: se calcula UNA vez al cargar porque depende sólo de
        // la malla y de sus clips, no de la pose ni del transform.
        obj.boundRadius    = skinnedBoundRadius(mesh);
        obj.hasBounds      = obj.boundRadius > 0.0f;
        // Tamaño de la malla en reposo, SOLO pa escalar el grosor del contorno.
        // No sirve boundRadius: esa cota vale para cualquier pose de cualquier
        // clip y es holgada a propósito (un brazo que llegue lejos la infla
        // varias veces por encima del cuerpo), así que usarla daba un borde
        // desproporcionado justo en las mallas con animación.
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

        // --- Flatten keyframes de TODOS los clips a formato GPU ---
        // (packSkinnedClips vive fuera pa poder probarse sin un VkDevice)
        const PackedClips packed = packSkinnedClips(mesh);
        obj.clipCount = (uint32_t)(mesh.animationClips.empty() ? 1u : mesh.animationClips.size());

        // --- Upload SSBOs estáticos ---
        m_res.uploadBuffer(packed.pos.data(),   packed.pos.size()   * sizeof(GpuPosKey),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, obj.keyframePosBuffer,   obj.keyframePosMemory,   batch);
        m_res.uploadBuffer(packed.rot.data(),   packed.rot.size()   * sizeof(GpuRotKey),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, obj.keyframeRotBuffer,   obj.keyframeRotMemory,   batch);
        m_res.uploadBuffer(packed.scale.data(), packed.scale.size() * sizeof(GpuPosKey),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, obj.keyframeScaleBuffer, obj.keyframeScaleMemory, batch);
        m_res.uploadBuffer(packed.boneInfos.data(), packed.boneInfos.size() * sizeof(GpuBoneInfo),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, obj.boneInfoBuffer,      obj.boneInfoMemory,      batch);
        m_res.uploadBuffer(mesh.skinnedVertices.data(), mesh.skinnedVertices.size() * sizeof(SkinnedVertex),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, obj.inputVertexBuffer,   obj.inputVertexMemory,   batch);

        // --- Index buffer ---
        createIndexBuffer(mesh.indices, obj.indexBuffer, obj.indexMemory, batch);

        // --- SSBOs dinámicos (device local, sin datos iniciales) ---
        m_res.createBuffer((uint32_t)boneCount * sizeof(glm::mat4),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
            obj.localTransformBuffer, obj.localTransformMemory);

        m_res.createBuffer((uint32_t)boneCount * sizeof(glm::mat4),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
            obj.finalBoneBuffer, obj.finalBoneMemory);

        // --- Output vertex buffer: SSBO + VB, stride 80 bytes (5×vec4) ---
        constexpr VkDeviceSize OUT_VERT = 5 * sizeof(glm::vec4);
        m_res.createBuffer((uint32_t)vertexCount * OUT_VERT,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
            obj.outputVertexBuffer, obj.outputVertexMemory);

        // --- Compute descriptor set ---
        // El pase encadena pools segun hacen falta, asi que esto ya no se cae
        // con el personaje 17. Se guarda de que pool salio: es lo que
        // destroySkinnedRenderObject necesita para devolverlo.
        obj.computeDescPool = m_skinningPass.allocateSet(skinningCtx(), obj.computeDescSet);
        if (obj.computeDescPool == VK_NULL_HANDLE)
            throw std::runtime_error("failed to allocate compute descriptor set!");

        VkDescriptorBufferInfo bufInfos[8]{};
        bufInfos[0] = { obj.keyframePosBuffer,    0, VK_WHOLE_SIZE };
        bufInfos[1] = { obj.keyframeRotBuffer,    0, VK_WHOLE_SIZE };
        bufInfos[2] = { obj.keyframeScaleBuffer,  0, VK_WHOLE_SIZE };
        bufInfos[3] = { obj.boneInfoBuffer,       0, VK_WHOLE_SIZE };
        bufInfos[4] = { obj.localTransformBuffer, 0, VK_WHOLE_SIZE };
        bufInfos[5] = { obj.finalBoneBuffer,      0, VK_WHOLE_SIZE };
        bufInfos[6] = { obj.inputVertexBuffer,    0, VK_WHOLE_SIZE };
        bufInfos[7] = { obj.outputVertexBuffer,   0, VK_WHOLE_SIZE };

        VkWriteDescriptorSet writes[8]{};
        for (int i = 0; i < 8; i++)
        {
            writes[i].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet          = obj.computeDescSet;
            writes[i].dstBinding      = (uint32_t)i;
            writes[i].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[i].descriptorCount = 1;
            writes[i].pBufferInfo     = &bufInfos[i];
        }
        vkUpdateDescriptorSets(m_gpu.device(), 8, writes, 0, nullptr);

        // --- Texturas y descriptor sets por material ---
        constexpr uint8_t white[4] = {255, 255, 255, 255};
        obj.matGfx.resize(mesh.materials.size());

        for (size_t mi = 0; mi < mesh.materials.size(); mi++)
        {
            const Material& smat = mesh.materials[mi];
            SkinnedMatGfx& mgfx = obj.matGfx[mi];

            // Diffuse
            m_res.createTextureImage(smat.texturePath, smat.embeddedTexture, mgfx.textureImage, mgfx.textureMem, batch);
            m_res.createTextureImageView(mgfx.textureImage, mgfx.textureView);
            m_res.createTextureSampler(mgfx.sampler);

            // Normal map
            m_res.createNormalMapImage(smat.normalMapPath, smat.embeddedNormalMap, mgfx.normalImage, mgfx.normalMem, batch);
            m_res.createTextureImageView(mgfx.normalImage, mgfx.normalView, VK_FORMAT_R8G8B8A8_UNORM);
            m_res.createTextureSampler(mgfx.normalSampler);

            // ORM
            if (!smat.metallicRoughnessPath.empty() || !smat.embeddedMetallicRoughness.empty())
            {
                m_res.createNormalMapImage(smat.metallicRoughnessPath, smat.embeddedMetallicRoughness,
                                     mgfx.ormImage, mgfx.ormMem, batch);
                mgfx.metallic  = 1.0f;
                mgfx.roughness = 1.0f;
            }
            else
            {
                m_res.createSolidColorImage(white, mgfx.ormImage, mgfx.ormMem, batch);
                mgfx.metallic  = smat.metallic;
                mgfx.roughness = smat.roughness;
            }
            m_res.createTextureImageView(mgfx.ormImage, mgfx.ormView, VK_FORMAT_R8G8B8A8_UNORM);
            m_res.createTextureSampler(mgfx.ormSampler);

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

                VkDescriptorImageInfo irrInfo{};
                irrInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                irrInfo.imageView   = m_iblPass.irradianceView();
                irrInfo.sampler     = m_iblPass.sampler();

                VkDescriptorImageInfo preInfo{};
                preInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                preInfo.imageView   = m_iblPass.prefilterView();
                preInfo.sampler     = m_iblPass.sampler();

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

                gw[5].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                gw[5].dstSet = mgfx.descSets[fi]; gw[5].dstBinding = 5;
                gw[5].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                gw[5].descriptorCount = 1; gw[5].pImageInfo = &irrInfo;

                gw[6].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                gw[6].dstSet = mgfx.descSets[fi]; gw[6].dstBinding = 6;
                gw[6].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                gw[6].descriptorCount = 1; gw[6].pImageInfo = &preInfo;

                vkUpdateDescriptorSets(m_gpu.device(), 7, gw, 0, nullptr);

                // Binding 7 (SSAO), igual que en la ruta estática: va aparte
                // porque su vista se rehace con el swapchain.
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

        // Espera a que la GPU termine: un command buffer en vuelo (double
        // buffering) puede estar leyendo los buffers que vamos a destruir.
        // Mismo motivo que en removeGameObject.
        vkDeviceWaitIdle(m_gpu.device());

        SkinnedRenderObject& obj = m_skinnedObjects[index];

        // El estado de animación es del Animator, no de los buffers: perderlo
        // haría que el personaje diera un salto visible al importar un fichero.
        const glm::mat4 transform  = obj.transform;
        const float     animTime   = obj.animTime;
        const uint32_t  activeClip = obj.activeClip;

        destroySkinnedRenderObject(obj);
        obj = SkinnedRenderObject{};
        initSkinnedRenderObject(obj, mesh);

        obj.transform = transform;
        obj.animTime  = animTime;
        // Clamp: la lista de clips puede haber encogido y activeClip apuntaría
        // fuera del SSBO de BoneInfos, con el compute leyendo basura en
        // silencio. Mismo criterio que setAnimationState.
        obj.activeClip = (activeClip < obj.clipCount) ? activeClip : 0;
    }

    void Renderer::updateAnimation(int index, float deltaTime)
    {
        if (index < 0 || index >= (int)m_skinnedObjects.size()) return;
        auto& obj = m_skinnedObjects[index];
        if (obj.ticksPerSecond <= 0.0f || obj.duration <= 0.0f) return;
        // Oculto: no se ve, así que su reloj tampoco corre. Al volver a marcarlo
        // Visible reanuda donde se quedó en vez de saltar hacia delante.
        if (!obj.meshVisible) return;
        obj.animTime += deltaTime * obj.ticksPerSecond;
        if (obj.animTime > obj.duration)
            obj.animTime = std::fmod(obj.animTime, obj.duration);
    }

    void Renderer::setAnimationState(int index, uint32_t clipIndex, float animTime)
    {
        if (index < 0 || index >= (int)m_skinnedObjects.size()) return;
        auto& obj = m_skinnedObjects[index];
        // Clamp y no assert: un clipIndex fuera de rango (escena con un grafo que
        // referencia un clip que el FBX ya no trae) haría que clipBase apuntara
        // fuera del SSBO de BoneInfos, y el compute leería basura en silencio.
        obj.activeClip = (clipIndex < obj.clipCount) ? clipIndex : 0;
        obj.animTime   = animTime;
        // Un objeto que deja de mezclar tiene que volver a peso 1 o el compute
        // seguiría leyendo el clip previo del frame anterior para siempre. Por
        // lo mismo se suelta el bloqueo de raíz: quien lo quiera pasa por
        // setAnimationBlend, que lo fija cada frame.
        obj.blendWeight = 1.0f;
        obj.lockRootMotion = false;
    }

    void Renderer::setAnimationBlend(int index, uint32_t clipIndex, float animTime,
                                     uint32_t prevClipIndex, float prevAnimTime, float weight,
                                     bool lockRootMotion)
    {
        setAnimationState(index, clipIndex, animTime);
        if (index < 0 || index >= (int)m_skinnedObjects.size()) return;
        auto& obj = m_skinnedObjects[index];
        // Mismo clamp que el clip activo: el clip previo también indexa el SSBO
        // de BoneInfos y un índice fuera de rango leería basura en silencio.
        obj.prevClip     = (prevClipIndex < obj.clipCount) ? prevClipIndex : 0;
        obj.prevAnimTime = prevAnimTime;
        obj.blendWeight  = (weight < 0.0f) ? 0.0f : (weight > 1.0f ? 1.0f : weight);
        obj.lockRootMotion = lockRootMotion;
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
        // Envía el batch pendiente (si lo hay) y lo pasa a m_inFlightBatches.
        flushPendingUploads();
        // Tras vkDeviceWaitIdle todas las fences de los batches en vuelo están
        // señaladas, así que cada uno es complete() y tickDeferredDeletes los
        // reclama y avanza m_lastCompletedTicket en una sola pasada: el bucle
        // termina en cuanto hasPendingUploads() es false. La espera es un stall
        // deliberado, aceptable en estas transiciones síncronas raras.
        vkDeviceWaitIdle(m_gpu.device());
        while (hasPendingUploads())
            tickDeferredDeletes();
    }

    void Renderer::tickDeferredDeletes()
    {
        m_deferredDeletes.tick(m_gpu.device());

        // Los batches se reclaman EN ORDEN estricto: un ticket solo se da por
        // completado cuando el suyo y todos los anteriores han señalado. Por eso
        // paramos en el primer batch del frente que aún no ha señalado, aunque uno
        // posterior ya lo haya hecho: si dejáramos que un batch posterior avanzara
        // m_lastCompletedTicket, un objeto de un batch anterior todavía en vuelo
        // (texturas aún en TRANSFER_DST_OPTIMAL) se volvería visible y samplearía
        // basura. Los batches se insertan con ticket creciente, así que el frente
        // es siempre el más antiguo.
        while (!m_inFlightBatches.empty() && m_inFlightBatches.front().batch->complete())
        {
            m_inFlightBatches.front().batch->reclaim();
            m_lastCompletedTicket = m_inFlightBatches.front().ticket;
            m_inFlightBatches.erase(m_inFlightBatches.begin());
        }
    }

    void Renderer::releaseRenderObject(RenderObject& obj)
    {
        // El objeto suelta su referencia YA: el resto del frame lo ve sin
        // recursos (el skip de recordCommandBuffer lo detecta por
        // sharedIndex < 0). Si quedan más holders no se destruye nada; si era
        // el último, la cache nos pasa una copia de los handles y la
        // destrucción de verdad ocurre kDelayFrames después.
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
        if (obj.sharedIndex < 0) return; // ya liberado
        releaseRenderObject(obj);
    }

    void Renderer::removeSkinnedObject(int index)
    {
        if (index < 0 || index >= (int)m_skinnedObjects.size()) return;
        SkinnedRenderObject& obj = m_skinnedObjects[index];
        if (obj.outputVertexBuffer == VK_NULL_HANDLE) return; // ya liberado
        // queueDestroy... ya deja obj vacío: la asignación de después sobra y
        // pisaría el snapshot si se dejara.
        queueDestroySkinnedRenderObject(obj);
    }

    void Renderer::removeGameObject(GameObject* node)
    {
        if (!node) return;
        // Sin vkDeviceWaitIdle: removeStaticObject/removeSkinnedObject encolan
        // la destrucción kDelayFrames frames, que es más de lo que cualquier
        // command buffer en vuelo puede tardar.
        node->traverse([this](GameObject* go) {
            if (go->staticRenderIndex >= 0)
                removeStaticObject(go->staticRenderIndex);
            if (go->skinnedRenderIndex >= 0)
                removeSkinnedObject(go->skinnedRenderIndex);
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
        // Sin vkDeviceWaitIdle: removeStaticObject/removeSkinnedObject encolan
        // la destrucción kDelayFrames frames, que es más de lo que cualquier
        // command buffer en vuelo puede tardar.
        if (go->staticRenderIndex >= 0)
            removeStaticObject(go->staticRenderIndex);
        go->staticRenderIndex = -1;
        // Desde que el import detecta rigs, el editor sí crea mallas skinned:
        // sin esto, quitar el componente filtra su render object en GPU y deja
        // el índice stale, que el resto del código toma por válido.
        if (go->skinnedRenderIndex >= 0)
            removeSkinnedObject(go->skinnedRenderIndex);
        go->skinnedRenderIndex = -1;
        go->setMesh(nullptr);
    }

    void Renderer::replaceStaticTextureWithMissing(int renderIndex, TextureSlot slot)
    {
        if (renderIndex < 0 || renderIndex >= (int)m_objects.size()) return;
        // La textura vive en la entrada compartida, así que el checkerboard lo
        // ven TODOS los objetos que comparten esa malla+material. Es lo
        // correcto: el asset que ha desaparecido es el mismo para todos ellos.
        SharedGpuMesh* gpuPtr = m_sharedMeshes.get(m_objects[renderIndex].sharedIndex);
        if (!gpuPtr) return;
        SharedGpuMesh& obj = *gpuPtr;

        // Sin vkDeviceWaitIdle: los handles viejos se encolan (ver más abajo) en
        // lugar de destruirse ya, así que un command buffer en vuelo que aún
        // referencie el descriptor set los sigue viendo válidos hasta que la
        // cola los libera kDelayFrames frames después.

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

        // Los cuatro handles viejos siguen referenciados por el descriptor set
        // que un command buffer en vuelo puede estar usando. Se encolan por
        // valor: capturar los punteros img/mem/view/sampler sería leer los
        // NUEVOS cuando el lambda corriera, tres frames después.
        const VkImage        oldImage   = *img;
        const VkDeviceMemory oldMem     = *mem;
        const VkImageView    oldView    = *view;
        const VkSampler      oldSampler = *sampler;
        m_deferredDeletes.push([oldImage, oldMem, oldView, oldSampler](VkDevice dev) {
            vkDestroySampler(dev,   oldSampler, nullptr);
            vkDestroyImageView(dev, oldView,    nullptr);
            vkDestroyImage(dev,     oldImage,   nullptr);
            vkFreeMemory(dev,       oldMem,     nullptr);
        });

        // path vacío + sin bytes embebidos = createTextureImage genera el
        // checkerboard "missing" de fallback (mismo camino que un modelo
        // cargado sin textura). createTextureImage crea la VkImage con
        // formato VK_FORMAT_R8G8B8A8_SRGB (hardcoded) para todos los slots;
        // por eso la image view se crea también en SRGB (formato por
        // defecto de createTextureImageView) para los tres slots, aunque
        // Normal/MetallicRoughness normalmente usen UNORM — la imagen no se
        // crea con VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT, así que la view debe
        // usar el mismo formato exacto con el que se creó la imagen o la
        // validation layer dispara VUID-VkImageViewCreateInfo-image-01762.
        m_res.createTextureImage("", {}, *img, *mem);
        m_res.createTextureImageView(*img, *view);
        m_res.createTextureSampler(*sampler);

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

    // ─── Offscreen images ────────────────────────────────────────────────────────

    void Renderer::createOffscreenImages()
    {
        // Sampler compartido entre los dos frames offscreen
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
            // Imagen color offscreen (LDR, ya tonemapeada). La escribe el pass de
            // composicion; la escena va a m_hdrImage.
            m_res.createImage(
                effectiveViewport().width, effectiveViewport().height,
                m_swapChainFormat,
                VK_IMAGE_TILING_OPTIMAL,
                // TRANSFER_SRC: origen del blit al swapchain en headless.
                VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                    VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                m_offscreenImage[i], m_offscreenMemory[i]);

            m_res.createTextureImageView(m_offscreenImage[i], m_offscreenView[i], m_swapChainFormat);

            // Framebuffer del pass de la UI: la MISMA imagen final, al tamano de
            // salida. Se crea aqui porque muere y renace con ella.
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

            // Imagen HDR de la escena: tamano INTERNO (que con SSAA es mayor que
            // el de la ventana), formato flotante. La leen el downsample del
            // bloom (compute) y la composicion (fragment). Con MSAA es el destino
            // del resolve, no el target directo del rasterizador.
            m_res.createImage(
                m_renderExtent.width, m_renderExtent.height,
                kHdrFormat,
                VK_IMAGE_TILING_OPTIMAL,
                // STORAGE ademas de SAMPLED: ssr_resolve.comp suma el reflejo
                // sobre esta misma imagen (imageLoad + imageStore del MISMO
                // texel) antes de que el bloom la lea. R16G16B16A16_SFLOAT es de
                // los formatos obligatorios como storage image.
                // TRANSFER_SRC: origen del blit a la cara del cubemap cuando se
                // bakea una reflection probe. No cambia nada del render normal.
                // TRANSFER_DST: destino de la copia de vuelta del motion blur,
                // que emborrona hacia una imagen aparte porque lee pixeles
                // arbitrarios. Apagado no se graba ninguna copia.
                VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                    VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                    VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                m_hdrImage[i], m_hdrMemory[i]);

            m_res.createTextureImageView(m_hdrImage[i], m_hdrView[i], kHdrFormat);

            // Registrar la textura en la capa de UI para obtener el
            // VkDescriptorSet. En headless nadie la muestrea: el descriptor
            // set se queda nulo y destroyOffscreenImages ya comprueba antes
            // de liberarlo.
            if (!m_headless && m_ui)
                m_offscreenDescSet[i] = m_ui->registerUiTexture(
                    reinterpret_cast<uint64_t>(m_offscreenSampler),
                    reinterpret_cast<uint64_t>(m_offscreenView[i]));
        }

        // ANTES de createAaImages: el descriptor set del TAA referencia
        // la profundidad del pre-pass. Sus tres imagenes van al
        // tamano interno del render, igual que el resto de targets intermedios.
        m_depthPrepass.createImages(depthPrepassCtx());
        m_ssaoPass.createImages(ssaoCtx());
        // Los sets por objeto guardan la vista vieja: hay que repasarlos. En el
        // arranque no hay ninguno todavia y el bucle no hace nada.
        refreshSsaoDescriptors();
        // DETRAS de createSsaoImages: el descriptor set del culling referencia
        // la profundidad, que se acaba de crear ahi. La rejilla se dimensiona con
        // m_renderExtent, igual que el resto de targets intermedios.
        m_fpPass.createBuffers(fpCtx());
        // ANTES de los framebuffers de escena y composicion: con MSAA sus
        // attachments de color son las imagenes multisample que se crean ahi.
        createMsaaImages();
        m_aaPass.createImages(aaCtx());

        const bool msaa = (m_aaSampleCount != VK_SAMPLE_COUNT_1_BIT);

        for (int i = 0; i < MAX_FRAMES; i++)
        {
            // Framebuffer de escena: HDR + depth compartido. Con MSAA se
            // rasteriza sobre el color multisample y el HDR de siempre pasa a ser
            // el destino del resolve, en el tercer slot.
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

            // Framebuffer de composicion: LDR + el MISMO depth (cargado). Solo
            // hace falta cuando la composicion escribe DIRECTAMENTE en la
            // offscreen; si el modo activo mete un pass de resolucion detras, el
            // destino es la imagen intermedia (m_aaSrcFramebuffer) y este no se
            // usaria. Ademas con SSAA seria invalido: el depth tiene el tamano
            // interno y la offscreen el de la ventana.
            if (needsAaIntermediate()) continue;

            VkImageView compAtts[3] = { msaa ? m_msaaLdrView[i] : m_offscreenView[i], m_depthImageView, m_offscreenView[i] };
            VkFramebufferCreateInfo compFb = fbInfo;
            compFb.renderPass      = m_compositeRenderPass;
            compFb.attachmentCount = msaa ? 3 : 2;
            compFb.pAttachments    = compAtts;
            if (vkCreateFramebuffer(m_gpu.device(), &compFb, nullptr, &m_compositeFramebuffer[i]) != VK_SUCCESS)
                throw std::runtime_error("failed to create composite framebuffer!");
        }

        // Depende de m_hdrView (los sets del primer downsample y de la
        // composicion lo referencian), asi que va despues del bucle.
        createBloomImages();
        // Depende de m_hdrView (el set de la suma lo referencia) y de
        // la profundidad del pre-pass (el de la marcha), asi que va detras de los dos.
        m_ssrPass.createImages(ssrCtx());
        // Depende de m_hdrView y de la profundidad del pre-pass igual que el SSR. En el
        // primer init sale por la guarda (el UBO aun no existe) y lo rehace el
        // final de init.
        m_fogPass.createSets(fogCtx());
        // Depende de m_hdrView y de la profundidad del pre-pass igual que el SSR.
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
            // Antes que la vista que referencia.
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

    // El pase de bloom vive en BloomPass; lo que queda aqui es la COMPOSICION,
    // que no es suya: suma el mip 0 sobre el HDR, tonemapea y ademas hospeda el
    // contorno de seleccion, los gizmos y la UI de juego.
    void Renderer::createBloomPipelines()
    {
        // --- Medicion del coste GPU -----------------------------------------
        // Propiedades del device que comparten todos los pases; se resuelven
        // aqui porque el bloom es el primero que pide un pool de queries.
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(m_gpu.physicalDevice(), &props);
        m_timestampPeriod     = props.limits.timestampPeriod;
        m_timestampsSupported = props.limits.timestampComputeAndGraphics && m_timestampPeriod > 0.0f;

        m_bloomPass.createPipelines(bloomCtx());

        // El layout de la composicion reutilizaba el VkDescriptorSetLayoutCreateInfo
        // del bloom: aqui hay que declararlo, con los mismos dos bindings.
        VkDescriptorSetLayoutCreateInfo dsl{};
        dsl.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        dsl.bindingCount = 2;

        // --- Composicion: escena HDR + mip 0 del bloom -----------------------
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

        // Sin vertex buffer: los tres vertices salen de gl_VertexIndex.
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
        // NONE: el triangulo se genera en el shader y su orientacion no depende
        // de ningun frontFace del resto del motor.
        compRs.cullMode    = VK_CULL_MODE_NONE;
        compRs.lineWidth   = 1.0f;

        VkPipelineMultisampleStateCreateInfo compMs{};
        compMs.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        // El triangulo de composicion tambien va al pass multisample cuando hay
        // MSAA: lo que se resuelve al final es el conjunto del pass, y este
        // pipeline tiene que declarar las mismas muestras que sus companeros.
        compMs.rasterizationSamples = m_aaSampleCount;

        // Sin test ni escritura de profundidad: el triangulo cubre la pantalla y
        // el depth cargado tiene que llegar INTACTO al contorno y a los gizmos,
        // que se dibujan justo despues en este mismo pass.
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
        // Los sets se crean SIEMPRE, haya cadena o no. Antes se salia aqui
        // cuando el viewport era diminuto (<4 px, que es cuando mipCount queda
        // a 0) y m_compositeSets se quedaba en VK_NULL_HANDLE... que es
        // exactamente lo que recordCommandBuffer bindea sin preguntar. Y el set
        // no es solo del bloom: su binding 0 es la escena HDR, sin la cual la
        // composicion -que es quien tonemapea- no tiene ni entrada.
        createCompositeSets();
    }

    // Los dos bindings que lee bloom_composite.frag: la escena HDR y el mip 0 de
    // la cadena del bloom. El sampler es el del bloom, que sirve para los dos.
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
            // Sin cadena no hay mip 0 al que apuntar, pero el descriptor tiene
            // que ser valido igualmente. Se repite la escena en el hueco del
            // bloom: recordCommandBuffer ya fuerza la intensidad a 0 cuando
            // mipCount es 0, asi que no suma nada. Ojo al layout, que NO es el
            // mismo: el mip vive en GENERAL y la escena en SHADER_READ_ONLY.
            //
            // Se repite la escena y no se inventa una imagen negra porque aqui
            // el sustituto es una imagen RENDERIZADA, no memoria sin escribir:
            // el caso peligroso del backend D3D12 (inf * 0 = NaN sobre un heap
            // sin poner a cero) no aplica.
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
        // Los sets de composicion mueren con el reset del pool que hace
        // createCompositeSets; aqui solo se anulan los handles.
        for (int f = 0; f < MAX_FRAMES; f++) m_compositeSets[f] = VK_NULL_HANDLE;
    }

    void Renderer::setBloomEnabled(bool v)
    {
        if (v == bloomEnabled()) return;
        setBloomEnabledFlag(v);
        // Al apagar, la cadena se queda con el bloom del último frame calculado y
        // la composición la sigue muestreando (el shader multiplica siempre, y
        // 0 * inf sería NaN). Un clear a negro por frame en vuelo la deja neutra;
        // a partir de ahí, cero trabajo.
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

    // ── Skinning por compute ────────────────────────────────────────────────
    SkinningPass::Context Renderer::skinningCtx()
    {
        // m_skinnedVisible es la MISMA lista que consume el bucle de dibujo: si
        // el pase saltara un objeto que luego se dibuja, le quedaria la pose del
        // ultimo frame en que fue visible.
        return SkinningPass::Context{ m_gpu, m_skinnedObjects, m_skinnedVisible };
    }

    // ── Shadow map ──────────────────────────────────────────────────────────
    ShadowPass::Context Renderer::shadowCtx()
    {
        // Los dos sets que declara shadow.vert. El pipeline layout que sale de
        // ellos lo presta el pase al depth pre-pass, que declara los mismos.
        return ShadowPass::Context{ m_gpu, m_descriptorSetLayout, m_instanceDescLayout };
    }

    // ── IBL global y sondas ─────────────────────────────────────────────────
    IblPass::Context Renderer::iblCtx()
    {
        // Sin skybox cargado las dos vistas van nulas y precompute() se sale:
        // los cubemaps se quedan con el ambiente neutro.
        return IblPass::Context{
            m_gpu,
            m_skybox.isInitialized() ? m_skybox.cubeView()    : VK_NULL_HANDLE,
            m_skybox.isInitialized() ? m_skybox.cubeSampler() : VK_NULL_HANDLE
        };
    }

    // El unico Context largo del motor, y por un motivo: el bake REDIBUJA la
    // escena. Todo lo que va aqui es del pass offscreen del frame (slot 0) y de
    // las listas de objetos, que se quedan en el Renderer.
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
            m_instanceDescSets[0], m_uniformBuffersMapped[0], m_uboWritten[0],
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
        // Viewport degenerado o recursos aun sin crear: nada que hacer.
        if (!m_ssaoPass.ready(m_currentFrame)) return;

        // El SSR come del MISMO depth pre-pass: con el SSAO apagado pero el SSR
        // activo hay que grabarlo igual. Lo unico que se desacopla es esto; los
        // dos dispatches de oclusion siguen atados a ssaoEnabled().
        // El TAA es el tercer cliente: reproyecta el frame anterior a partir de
        // esta misma profundidad, y la quiere SIN el jitter de subpixel, que es
        // justo como la graba este pre-pass (usa fc.proj, no la jittereada).
        // El cuarto cliente es el Forward+ tiled: reduce el maximo de profundidad
        // de cada tile a partir de esta misma imagen. El clustered NO la necesita
        // (su rejilla es analitica) y por eso no entra aqui.
        // El quinto cliente es la niebla volumetrica: desproyecta esta misma
        // profundidad para saber hasta donde marchar cada pixel. Sin esto, con
        // la niebla encendida y todo lo demas apagado, la imagen de profundidad
        // no se grabaria en el frame.
        // El sexto es el motion blur: reproyecta esta misma profundidad al frame
        // anterior para sacar la velocidad de cada pixel. Sin esto, encendido y
        // con todo lo demas apagado, leeria una imagen que nadie ha escrito.
        const bool ssrNeedsDepth = m_ssrPass.active(ssrCtx()) || m_aaActiveMode == AaMode::Taa ||
                                   m_fpActiveMode == FpMode::Tiled || m_fogEnabled ||
                                   m_motionBlurPass.active(motionBlurCtx());
        m_ssrStampedPrepass = false;

        // Apagado: deja el mapa en la identidad si hay algo que limpiar.
        // Encendido: lee los timestamps del slot y abre el par de este frame.
        m_ssaoPass.recordPreDepth(ssaoCtx(), cmd);

        // Con el SSAO apagado y sin nadie mas pidiendo la profundidad, aquí
        // acaba el frame para este pass: cero trabajo grabado.
        if (!ssaoEnabled() && !ssrNeedsDepth) return;

        // Queries del SSR: se resetean las CUATRO aquí, que es lo primero suyo
        // que se graba en el frame, y el par [0,1] acota el pre-pass. Con el SSAO
        // encendido ese coste ya lo mide su propio par y recordSsrPass no lo
        // vuelve a sumar; el par se escribe igualmente para que la lectura de los
        // cuatro nunca dé NOT_READY.
        if (ssrNeedsDepth && m_timestampsSupported)
        {
            vkCmdResetQueryPool(cmd, m_ssrPass.queryPool(), m_currentFrame * 4, 4);
            vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, m_ssrPass.queryPool(), m_currentFrame * 4);
            m_ssrStampedPrepass = true;
        }

        // ── Depth pre-pass: la escena entera, solo profundidad ───────────────
        // El target y el pipeline son de DepthPrepassPass; los draws se quedan
        // aqui, que es donde estan las listas de objetos y el SSBO de
        // instancias.
        {
            m_depthPrepass.begin(depthPrepassCtx(), cmd);

            // Mismas guardas y mismo frustum que el pass de escena: si aquí
            // entrara algo que allí no se dibuja, el AO oscurecería contra
            // geometría invisible.
            //
            // Los personajes SÍ entran, detrás de los estáticos. Antes no, con el
            // argumento de que el pass de sombras tampoco los metía — y eso era
            // falso: sí los dibuja (ver bindSkinnedPipeline más abajo). Esta
            // profundidad no es solo del AO: la NIEBLA marcha hasta ella, así que
            // sin los personajes la niebla de su silueta se calculaba hasta lo que
            // hubiera detrás y salía un recorte con la forma del objeto de atrás.
            m_batchCandidates.clear();
            m_batchCandidates.reserve(m_objects.size());
            for (auto& obj : m_objects)
            {
                const SharedGpuMesh* gpu = m_sharedMeshes.get(obj.sharedIndex);
                bool visible = obj.meshVisible && gpu && gpu->uploadTicket <= m_lastCompletedTicket;
                if (visible && gpu->hasBounds &&
                    !aabbVisible(camFrustum, gpu->aabbMin, gpu->aabbMax, obj.transform))
                {
                    visible = false;
                }
                m_batchCandidates.push_back({ obj.sharedIndex, visible, &obj.transform });
            }

            // Tramo propio del SSBO, detrás del de las cascadas y delante del del
            // pass de escena.
            const uint32_t instanceBase = m_instanceCursor;
            glm::mat4* dst = (glm::mat4*)m_instanceMapped[m_currentFrame] + instanceBase;
            m_instanceCursor += buildInstanceBatches(m_batchCandidates.data(), m_batchCandidates.size(),
                dst, m_instanceCapacity[m_currentFrame] - instanceBase, instanceBase, m_instanceBatches);

            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_shadowPass.pipelineLayout(),
                1, 1, &m_instanceDescSets[m_currentFrame], 0, nullptr);

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

            // Y los personajes, con su propio pipeline: lo que se dibuja es la
            // SALIDA del compute de skinning, que tiene otro stride. Mismas
            // guardas que en el pass de sombras: si el compute no despachó para
            // este objeto, su buffer lleva una pose vieja.
            {
                bool skinnedBound = false;
                for (size_t si = 0; si < m_skinnedObjects.size(); si++)
                {
                    // La MISMA lista de visibles que consumió el compute, igual que
                    // en el pass de sombras: a un objeto sin despachar le quedaría
                    // la pose del último frame en que fue visible.
                    if (si >= m_skinnedVisible.size() || !m_skinnedVisible[si]) continue;
                    const SkinnedRenderObject& sobj = m_skinnedObjects[si];
                    if (!sobj.meshVisible) continue;
                    if (sobj.outputVertexBuffer == VK_NULL_HANDLE || sobj.matGfx.empty())
                        continue;
                    // Sin sitio en el SSBO de este frame: mejor sin profundidad que
                    // pisar el tramo de otro pase.
                    if (m_instanceCursor >= m_instanceCapacity[m_currentFrame]) break;

                    // El shader saca el model del SSBO por gl_InstanceIndex: una
                    // entrada por objeto y un draw de una instancia con
                    // firstInstance apuntando a ella.
                    const uint32_t instanceIndex = m_instanceCursor++;
                    ((glm::mat4*)m_instanceMapped[m_currentFrame])[instanceIndex] =
                        sobj.transform;

                    if (!skinnedBound)
                    {
                        m_depthPrepass.bindSkinnedPipeline(cmd);
                        skinnedBound = true;
                    }

                    // Set 0 solo por el UBO: este shader no muestrea nada, así que
                    // vale cualquier set del layout que declara el pipeline.
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

        // El SSR solo quería la profundidad: sin SSAO no hay oclusión que
        // calcular ni mapa que escribir.
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
        // El recorrido se queda aqui: m_objects y m_skinnedObjects son del
        // Renderer, no del pase. SsrPass::active() recibe el resultado.
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

    // ── Niebla volumetrica ──────────────────────────────────────────────────
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
            m_aaPass.currViewProj(), m_aaPass.prevViewProj()
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

    // Los dos pools de timestamps que se crean con el AA. El del AA se queda
    // aqui porque ademas cronometra el frame entero sin UI ([2,3]), y el del
    // panel Performance porque este es el ultimo sitio del arranque donde
    // m_timestampsSupported ya esta resuelto y el device sigue vivo.
    void Renderer::createAaQueryPools()
    {
        if (!m_timestampsSupported) return;

        VkQueryPoolCreateInfo qpi{};
        qpi.sType      = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        qpi.queryType  = VK_QUERY_TYPE_TIMESTAMP;
        qpi.queryCount = MAX_FRAMES * 4;
        if (vkCreateQueryPool(m_gpu.device(), &qpi, nullptr, &m_aaQueryPool) != VK_SUCCESS)
            throw std::runtime_error("failed to create aa query pool!");

        // Pool del panel Performance: [0,1] sombras, [2,3] escena. Con el panel
        // cerrado no se usa ni una query.
        qpi.queryCount = MAX_FRAMES * 4;
        if (vkCreateQueryPool(m_gpu.device(), &qpi, nullptr, &m_perfQueryPool) != VK_SUCCESS)
            throw std::runtime_error("failed to create perf query pool!");
    }

    // Los targets multisample de la escena y de la composicion. NO son del pase
    // de resolucion: el resolve del MSAA ocurre dentro de esos dos render
    // passes, que son del Renderer.
    void Renderer::createMsaaImages()
    {
        if (m_aaActiveMode != AaMode::Msaa) return;

        // Imagen multisample: GpuResources::createImage fija samples = 1, asi que
        // estas dos van a mano. Ninguna se muestrea ni se blitea nunca: solo
        // sirven de attachment y se resuelven dentro del render pass.
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
            // Color multisample de la escena y de la composicion. Los dos se
            // resuelven dentro de su render pass sobre las imagenes de una
            // muestra de siempre, asi que nada de lo que hay detras (SSAO,
            // SSR, bloom, UI, blit) se entera de que existen.
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
        // Cualquier cambio de modo mueve recursos: el tamano interno (SSAA), el
        // numero de muestras (MSAA), o simplemente la existencia de la imagen
        // intermedia y del historial. Se reconstruye entero, que es barato de
        // razonar y ocurre una vez por click del usuario.
        m_aaResourcesDirty = true;
    }

    void Renderer::setViewportSize(uint32_t width, uint32_t height)
    {
        // Panel colapsado o con area nula: no hay nada que renderizar y crear
        // imagenes de 0 pixeles es invalido. Se conserva el tamano anterior.
        if (width == 0 || height == 0) return;
        if (m_viewportExtent.width == width && m_viewportExtent.height == height) return;
        m_viewportExtent   = { width, height };
        // Misma via que un cambio de modo: recrear con la GPU en reposo, al
        // principio del frame siguiente.
        m_aaResourcesDirty = true;
    }

    void Renderer::setSsaaFactor(float v)
    {
        if (v == ssaaFactor()) return;
        setSsaaFactorFlag(v);
        // Solo cambia el tamano de los targets cuando SSAA es el modo activo.
        if (aaMode() == AaMode::Ssaa) m_aaResourcesDirty = true;
    }

    void Renderer::setShadowResolution(int v)
    {
        if (v == shadowResolution() || v <= 0) return;
        setShadowResolutionFlag(v);
        // No se rehace aqui: esto lo llama la UI en mitad de un frame, y soltar
        // el shadow map ahora mismo lo quitaria de debajo de la lista de
        // comandos en vuelo. Se marca y drawFrame lo atiende entre frames.
        m_shadowResourcesDirty = true;
    }

    void Renderer::rebuildShadowResources()
    {
        m_shadowResourcesDirty = false;

        // La imagen puede estar en el frame anterior, que todavia no ha
        // terminado. Es un ajuste de calidad que se toca de uvas a peras: un
        // wait completo sale mas barato que llevar borrado diferido para esto.
        vkDeviceWaitIdle(m_gpu.device());

        m_shadowPass.resizeResources(shadowCtx(), (uint32_t)shadowResolution());

        // Y TODO lo que apuntaba a la vista vieja. Son tres consumidores y hay
        // que acordarse de los tres: los sets de cada malla y los de cada
        // material de personaje (binding 3), que rehace refreshShadowDescriptors,
        // y los del pase de NIEBLA, que se lleva la vista y el sampler en su
        // Context (ver fogCtx) y por tanto tambien se queda con la vista muerta.
        //
        // Olvidar la niebla dejaba un imageView destruido en un set de compute:
        // el arranque se llenaba de errores de validacion en CADA frame.
        refreshShadowDescriptors();
        // createSets resetea su pool antes de repartir, asi que re-llamarlo no
        // lo agota.
        m_fogPass.createSets(fogCtx());
    }

    void Renderer::refreshShadowDescriptors()
    {
        VkDescriptorImageInfo shadowInfo{};
        shadowInfo.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
        shadowInfo.imageView   = m_shadowPass.view();
        shadowInfo.sampler     = m_shadowPass.sampler();

        // El sampler NO cambia en un resize (resizeResources no lo toca), pero
        // se reescribe igual: el write es uno solo y asi esta funcion vale
        // tambien si algun dia el sampler pasa a depender del tamano.
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

        // Mallas estaticas: por ENTRADA COMPARTIDA, que es de quien son los
        // sets (varios objetos con la misma malla comparten uno).
        for (int index : m_sharedMeshes.liveIndices())
        {
            SharedGpuMesh* mesh = m_sharedMeshes.get(index);
            if (!mesh) continue;
            for (int f = 0; f < MAX_FRAMES; f++) writeBinding3(mesh->descriptorSets[f]);
        }

        // Personajes: un bloque por material, que es como se dibujan.
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
        // El color y la profundidad tienen que coincidir: el pass de escena usa
        // los dos a la vez, asi que el maximo util es la interseccion.
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
            // maxFramebufferWidth/Height nunca son menores que maxImageDimension2D,
            // asi que con recortar por este limite basta para los dos.
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
        // Solo los que viven en el pass de escena y en el de composicion. Los de
        // sombras, los del depth pre-pass y los de resolucion del AA tienen
        // render passes propios que siempre van a una muestra.
        VkPipeline* pipelines[] = {
            &m_pipeline, &m_wireframePipeline, &m_outlinePipeline, &m_outlineWirePipeline,
            &m_skinnedGfxPipeline, &m_skinnedWireframePipeline,
            &m_skinnedOutlinePipeline, &m_skinnedOutlineWirePipeline,
            &m_compositePipeline,
            // Los tres compute del skinning YA NO estan aqui: viven en
            // SkinningPass, no dependen de las muestras y su creacion ya no va
            // en la misma pasada que estos, asi que no hay que destruirlos ni
            // rehacerlos al cambiar de MSAA.
        };
        for (VkPipeline* p : pipelines)
        {
            if (*p != VK_NULL_HANDLE)
            {
                vkDestroyPipeline(m_gpu.device(), *p, nullptr);
                *p = VK_NULL_HANDLE;
            }
        }

        createPipeline();
        createSkinnedGraphicsPipelines();

        // El pipeline de composicion vive en createBloomPipelines, que ademas
        // crea layouts y pools; aqui solo hace falta el pipeline, asi que se
        // rehace a mano con el mismo codigo que usa aquella.
        recreateCompositePipeline();
        // La UI de PANTALLA no se rehace aqui: tiene pass propio, a una muestra
        // siempre, y el numero de muestras de la escena no le afecta.
        //
        // La de MUNDO si, y es obligatorio: sus dos variantes estan compiladas
        // contra m_offscreenRenderPass, que el llamante (rebuildAaResources)
        // acaba de destruir y volver a crear con otro numero de muestras. Un
        // pipeline que apunta a un VkRenderPass destruido NO da error de
        // validacion — se manifiesta como device lost la primera vez que se usa.
        // Esta es la UNICA ruta de recreacion: createOffscreenRenderPass() solo
        // se llama en dos sitios (el init y ese bloque de rebuildAaResources), y
        // recreateSwapChain no lo toca.
        m_uiBatch.initWorldPipelines(m_gpu, m_offscreenRenderPass, m_aaSampleCount);

        // Los dos que no viven en Renderer.cpp. El skybox se salta solo si no
        // hay cubemap cargado.
        m_skybox.recreatePipeline(m_gpu, m_offscreenRenderPass, m_aaSampleCount);
        Gizmos::recreatePipeline(m_gpu, m_compositeRenderPass, m_aaSampleCount);
    }

    void Renderer::rebuildAaResources()
    {
        m_aaResourcesDirty = false;

        // Lo pedido pasa a ser lo construido ANTES de tocar nada: todo lo que hay
        // debajo (targetSampleCount, needsAaIntermediate, updateRenderExtent,
        // createAaImages) decide que crear mirando estos dos. A partir de aqui
        // coinciden hasta el proximo click o el proximo arrastre del panel.
        m_aaActiveMode   = aaMode();
        m_viewportActive = m_viewportExtent;

        // Nada de esto se puede tocar con trabajo en vuelo: son imagenes, render
        // passes y pipelines que la GPU puede estar leyendo ahora mismo.
        vkDeviceWaitIdle(m_gpu.device());

        const VkSampleCountFlagBits wanted = targetSampleCount();
        const bool samplesChanged = (wanted != m_aaSampleCount);

        destroyOffscreenImages();

        if (samplesChanged)
        {
            m_aaSampleCount = wanted;
            // Los render passes declaran el numero de muestras en cada
            // attachment, y los pipelines tienen que coincidir con su pass.
            vkDestroyRenderPass(m_gpu.device(), m_offscreenRenderPass, nullptr);
            vkDestroyRenderPass(m_gpu.device(), m_compositeRenderPass, nullptr);
            createOffscreenRenderPass();
            createCompositeRenderPass();
            recreateMsaaDependentPipelines();
        }

        // El depth lo comparten el pass de escena y el de composicion, asi que
        // cambia tanto con el tamano (SSAA) como con las muestras (MSAA).
        vkDestroyImageView(m_gpu.device(), m_depthImageView, nullptr);
        vkDestroyImage(m_gpu.device(), m_depthImage, nullptr);
        vkFreeMemory(m_gpu.device(), m_depthImageMemory, nullptr);

        updateRenderExtent();
        createDepthResources();
        createOffscreenImages();

        // Los descriptor sets de los objetos referencian el mapa de AO, que
        // acaba de recrearse con otro tamano.
        refreshSsaoDescriptors();
    }

}
