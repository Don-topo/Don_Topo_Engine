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
        // Los gizmos van en el pass de composicion, no en el de escena: ahi el
        // color ya esta tonemapeado y sus lineas salen exactamente con el color
        // plano que declaran, igual que antes del bloom.
        Gizmos::init(m_gpu, m_compositeRenderPass, m_swapChainFormat, m_aaSampleCount);
        // Passes del AA: solo dependen de m_swapChainFormat, igual que el de
        // composicion, asi que sobreviven a los resize (lo que se recrea son sus
        // imagenes y framebuffers, en createAaImages).
        createAaRenderPasses();
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
        createFpPipelines();
        createPipeline();
        createShadowResources();
        createComputePipelines();
        // ANTES de createDescriptorSets: los sets de cada objeto escriben ya las
        // vistas de los dos cubemaps del IBL (bindings 5 y 6). Aqui se crean con
        // contenido neutro; initSkybox los rellenara si hay entorno.
        createIblResources();
        // ANTES de createOffscreenImages: ahi se crea la cadena de mips, que
        // necesita el descriptor set layout y el pool del bloom ya montados.
        createBloomPipelines();
        // UI de juego: mismo pass y mismas muestras que la composicion, que es
        // donde se graban sus lotes (LDR, ya tonemapeado, encima de la escena).
        m_uiBatch.init(m_gpu, m_res, m_compositeRenderPass, m_aaSampleCount);
        // ANTES de createOffscreenImages (que llama a createSsaoImages) y DESPUÉS
        // de createShadowResources: el pipeline del depth pre-pass reutiliza
        // m_shadowPipelineLayout, que se crea allí.
        createSsaoPipelines();
        // Detrás del SSAO: comparte su sampler de profundidad (m_ssaoSampler) y
        // su depth pre-pass, y el pool de queries se apoya en el
        // m_timestampsSupported que resolvió el bloom.
        createSsrPipelines();
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
        createAaPipelines();
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

        // Reflection probes: MISMO sitio y mismo motivo que la linea de arriba.
        // Aqui se puede esperar a que la GPU quede libre para bakear una sonda o
        // reescribir los bindings 5/6 de un descriptor set. Sin sondas en la
        // escena sale por el camino rapido sin tocar nada, y con sondas ya
        // bakeadas y quietas tampoco graba un solo comando: el coste GPU por
        // frame es identico en los tres casos.
        syncReflectionProbes();

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

        computeCascades();
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
        vkDestroyPipeline(m_gpu.device(), m_bloomDownPipeline, nullptr);
        vkDestroyPipeline(m_gpu.device(), m_bloomUpPipeline, nullptr);
        vkDestroyPipelineLayout(m_gpu.device(), m_bloomPipelineLayout, nullptr);
        vkDestroyDescriptorPool(m_gpu.device(), m_bloomDescPool, nullptr);
        vkDestroyDescriptorSetLayout(m_gpu.device(), m_bloomDescLayout, nullptr);
        vkDestroySampler(m_gpu.device(), m_bloomSampler, nullptr);
        if (m_bloomQueryPool != VK_NULL_HANDLE)
        {
            vkDestroyQueryPool(m_gpu.device(), m_bloomQueryPool, nullptr);
            m_bloomQueryPool = VK_NULL_HANDLE;
        }
        // SSAO. Las imagenes, vistas, framebuffers y sets se fueron con
        // destroyOffscreenImages (llama a destroySsaoImages); aqui queda lo que
        // no depende del tamano. El pipeline layout es el del pass de sombras y
        // se destruye con el, mas abajo.
        vkDestroyPipeline(m_gpu.device(), m_ssaoPipeline, nullptr);
        vkDestroyPipeline(m_gpu.device(), m_ssaoBlurPipeline, nullptr);
        vkDestroyPipeline(m_gpu.device(), m_ssaoDepthPipeline, nullptr);
        vkDestroyPipelineLayout(m_gpu.device(), m_ssaoPipelineLayout, nullptr);
        vkDestroyDescriptorPool(m_gpu.device(), m_ssaoDescPool, nullptr);
        vkDestroyDescriptorSetLayout(m_gpu.device(), m_ssaoDescLayout, nullptr);
        vkDestroySampler(m_gpu.device(), m_ssaoSampler, nullptr);
        vkDestroyRenderPass(m_gpu.device(), m_ssaoDepthRenderPass, nullptr);
        m_ssaoDepthRenderPass = VK_NULL_HANDLE;
        if (m_ssaoQueryPool != VK_NULL_HANDLE)
        {
            vkDestroyQueryPool(m_gpu.device(), m_ssaoQueryPool, nullptr);
            m_ssaoQueryPool = VK_NULL_HANDLE;
        }

        // SSR: las imagenes y los sets ya se fueron con destroyOffscreenImages;
        // aqui solo queda lo que es independiente del tamano.
        vkDestroyPipeline(m_gpu.device(), m_ssrPipeline, nullptr);
        vkDestroyPipeline(m_gpu.device(), m_ssrResolvePipeline, nullptr);
        vkDestroyPipelineLayout(m_gpu.device(), m_ssrPipelineLayout, nullptr);
        vkDestroyDescriptorPool(m_gpu.device(), m_ssrDescPool, nullptr);
        vkDestroyDescriptorSetLayout(m_gpu.device(), m_ssrDescLayout, nullptr);
        vkDestroySampler(m_gpu.device(), m_ssrSampler, nullptr);
        if (m_ssrQueryPool != VK_NULL_HANDLE)
        {
            vkDestroyQueryPool(m_gpu.device(), m_ssrQueryPool, nullptr);
            m_ssrQueryPool = VK_NULL_HANDLE;
        }

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
        vkDestroyPipeline(m_gpu.device(), m_fpTiledPipeline, nullptr);
        vkDestroyPipeline(m_gpu.device(), m_fpClusteredPipeline, nullptr);
        vkDestroyPipelineLayout(m_gpu.device(), m_fpPipelineLayout, nullptr);
        vkDestroyDescriptorPool(m_gpu.device(), m_fpDescPool, nullptr);
        vkDestroyDescriptorSetLayout(m_gpu.device(), m_fpDescLayout, nullptr);
        for (int f = 0; f < MAX_FRAMES; f++)
        {
            vkDestroyBuffer(m_gpu.device(), m_fpParamsBuffer[f], nullptr);
            vkFreeMemory(m_gpu.device(), m_fpParamsMemory[f], nullptr);
            vkDestroyBuffer(m_gpu.device(), m_fpLightBuffer[f], nullptr);
            vkFreeMemory(m_gpu.device(), m_fpLightMemory[f], nullptr);
            vkDestroyBuffer(m_gpu.device(), m_fpStatsBuffer[f], nullptr);
            vkFreeMemory(m_gpu.device(), m_fpStatsMemory[f], nullptr);
        }
        if (m_fpQueryPool != VK_NULL_HANDLE)
        {
            vkDestroyQueryPool(m_gpu.device(), m_fpQueryPool, nullptr);
            m_fpQueryPool = VK_NULL_HANDLE;
        }

        // Anti-aliasing: las imagenes, los framebuffers y los sets se fueron con
        // destroyOffscreenImages (llama a destroyAaImages); aqui queda lo que no
        // depende del tamano ni del modo.
        vkDestroyPipeline(m_gpu.device(), m_fxaaPipeline, nullptr);
        vkDestroyPipeline(m_gpu.device(), m_ssaaPipeline, nullptr);
        vkDestroyPipeline(m_gpu.device(), m_taaPipeline, nullptr);
        vkDestroyPipelineLayout(m_gpu.device(), m_fxaaPipelineLayout, nullptr);
        vkDestroyPipelineLayout(m_gpu.device(), m_ssaaPipelineLayout, nullptr);
        vkDestroyPipelineLayout(m_gpu.device(), m_taaPipelineLayout, nullptr);
        vkDestroyDescriptorPool(m_gpu.device(), m_aaDescPool, nullptr);
        vkDestroyDescriptorPool(m_gpu.device(), m_taaDescPool, nullptr);
        vkDestroyDescriptorSetLayout(m_gpu.device(), m_aaDescLayout, nullptr);
        vkDestroyDescriptorSetLayout(m_gpu.device(), m_taaDescLayout, nullptr);
        vkDestroySampler(m_gpu.device(), m_aaSampler, nullptr);
        vkDestroyRenderPass(m_gpu.device(), m_aaRenderPass, nullptr);
        vkDestroyRenderPass(m_gpu.device(), m_taaHistoryRenderPass, nullptr);
        m_aaRenderPass         = VK_NULL_HANDLE;
        m_taaHistoryRenderPass = VK_NULL_HANDLE;
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
        // m_descriptorPool se destruye más abajo, DESPUÉS de los dos bucles que
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
        // Shadow Map
        vkDestroySampler(m_gpu.device(), m_shadowSampler, nullptr);
        vkDestroyImageView(m_gpu.device(), m_shadowView, nullptr);
        for (int c = 0; c < SHADOW_CASCADES; c++)
        {
            vkDestroyImageView(m_gpu.device(), m_shadowLayerViews[c], nullptr);
            vkDestroyFramebuffer(m_gpu.device(), m_shadowFramebuffers[c], nullptr);
        }
        vkDestroyImage(m_gpu.device(), m_shadowImage, nullptr);
        vkFreeMemory(m_gpu.device(), m_shadowMemory, nullptr);
        // IBL
        vkDestroyPipeline(m_gpu.device(), m_iblIrradiancePipeline, nullptr);
        vkDestroyPipeline(m_gpu.device(), m_iblPrefilterPipeline, nullptr);
        vkDestroyPipelineLayout(m_gpu.device(), m_iblPipelineLayout, nullptr);
        vkDestroyDescriptorPool(m_gpu.device(), m_iblDescPool, nullptr);
        vkDestroyDescriptorSetLayout(m_gpu.device(), m_iblDescLayout, nullptr);
        vkDestroySampler(m_gpu.device(), m_iblSampler, nullptr);
        vkDestroyImageView(m_gpu.device(), m_iblIrradianceView, nullptr);
        vkDestroyImageView(m_gpu.device(), m_iblIrradianceStore, nullptr);
        vkDestroyImage(m_gpu.device(), m_iblIrradianceImage, nullptr);
        vkFreeMemory(m_gpu.device(), m_iblIrradianceMemory, nullptr);
        vkDestroyImageView(m_gpu.device(), m_iblPrefilterView, nullptr);
        for (uint32_t m = 0; m < IBL_PREFILTER_MIPS; m++)
            vkDestroyImageView(m_gpu.device(), m_iblPrefilterStore[m], nullptr);
        vkDestroyImage(m_gpu.device(), m_iblPrefilterImage, nullptr);
        vkFreeMemory(m_gpu.device(), m_iblPrefilterMemory, nullptr);
        // Reflection probes. El cubemap de captura y el query pool solo existen
        // si alguna vez se bakeo algo; las sondas, si la escena tenia alguna.
        for (GpuProbe& probe : m_probes) destroyProbeImages(probe);
        m_probes.clear();
        if (m_probeCaptureView != VK_NULL_HANDLE)
        {
            vkDestroyImageView(m_gpu.device(), m_probeCaptureView, nullptr);
            vkDestroyImage(m_gpu.device(), m_probeCaptureImage, nullptr);
            vkFreeMemory(m_gpu.device(), m_probeCaptureMemory, nullptr);
            m_probeCaptureView = VK_NULL_HANDLE;
        }
        if (m_probeQueryPool != VK_NULL_HANDLE)
        {
            vkDestroyQueryPool(m_gpu.device(), m_probeQueryPool, nullptr);
            m_probeQueryPool = VK_NULL_HANDLE;
        }
        vkDestroyPipeline(m_gpu.device(), m_skinnedGfxPipeline, nullptr);
        vkDestroyPipeline(m_gpu.device(), m_skinnedWireframePipeline, nullptr);
        vkDestroyPipeline(m_gpu.device(), m_skinnedOutlinePipeline, nullptr);
        vkDestroyPipeline(m_gpu.device(), m_skinnedOutlineWirePipeline, nullptr);
        vkDestroyPipeline(m_gpu.device(), m_shadowPipeline, nullptr);
        vkDestroyPipeline(m_gpu.device(), m_shadowSkinnedPipeline, nullptr);
        vkDestroyPipelineLayout(m_gpu.device(), m_shadowPipelineLayout, nullptr);
        vkDestroyRenderPass(m_gpu.device(), m_shadowRenderPass, nullptr);
        for (auto& obj : m_skinnedObjects)
        {
            destroySkinnedRenderObject(obj);
        }

        m_skinnedObjects.clear();
        // Ahora sí: ya no queda ningún destroySkinnedRenderObject pendiente que
        // necesite liberar sets de m_descriptorPool.
        vkDestroyDescriptorPool(m_gpu.device(), m_descriptorPool, nullptr);
        if (m_computeDescPool != VK_NULL_HANDLE)
        {
            vkDestroyDescriptorPool(m_gpu.device(), m_computeDescPool, nullptr);
        }
        vkDestroyPipeline(m_gpu.device(), m_boneEvalPipeline,      nullptr);
        vkDestroyPipeline(m_gpu.device(), m_boneHierarchyPipeline, nullptr);
        vkDestroyPipeline(m_gpu.device(), m_skinningPipeline,       nullptr);
        vkDestroyPipelineLayout(m_gpu.device(), m_computePipelineLayout, nullptr);
        vkDestroyDescriptorSetLayout(m_gpu.device(), m_computeDescLayout, nullptr);
        m_skybox.shutdown(m_gpu);
        m_splash.shutdown(m_gpu);
        Gizmos::shutdown(m_gpu);
        // Los atlas ANTES del batch: sus descriptor sets salen de su pool, y
        // destruir el pool primero dejaria los handles colgando.
        for (auto& atlas : m_uiAtlases) atlas->destroy(m_gpu);
        m_uiAtlases.clear();
        for (auto& font : m_uiFonts) font->destroy(m_gpu);
        m_uiFonts.clear();
        m_uiBatch.shutdown(m_gpu);
        printf("destroy render items OK\n"); fflush(stdout);
        m_gpu.shutdown();
    }

    UiTextureAtlas* Renderer::loadUiAtlas(const std::string& path)
    {
        auto atlas = std::make_unique<UiTextureAtlas>();
        if (!atlas->loadFromFile(m_gpu, m_res, path)) return nullptr;
        if (!m_uiBatch.registerAtlas(m_gpu, *atlas))
        {
            atlas->destroy(m_gpu);
            return nullptr;
        }
        m_uiAtlases.push_back(std::move(atlas));
        return m_uiAtlases.back().get();
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

    void Renderer::initSkybox(const std::array<std::string, 6>& facePaths)
    {
        // kHdrFormat: el skybox dibuja en el pass de escena, que desde el bloom
        // sale en flotante. Su color pasa por el tonemap de la composicion como
        // el resto de la escena — y es lo que permite que el cielo genere bloom.
        m_skybox.init(m_gpu, m_offscreenRenderPass, kHdrFormat, facePaths, m_aaSampleCount);
        // El cubemap recien cargado es la fuente del IBL. Una sola vez, aqui: es
        // el punto por el que pasan tanto el editor como DonTopoRuntime, y no
        // depende de nada del editor.
        precomputeIbl();
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

    // Las dos delegan en Renderer/Frustum.h, que es donde vive la geometria
    // desde que hay un segundo backend que la usa. Se quedan porque son la
    // puerta por la que entran los tests y el resto de este fichero.
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

    // Mayor factor por el que la 3x3 de m puede estirar un vector, o sea su
    // mayor valor singular. Sirve para acotar cuánto separa una matriz de hueso
    // a un vértice del origen de ese hueso.
    //
    // Se calcula exacto (autovalores de m^T·m por la forma cerrada de una
    // simétrica 3x3) en vez de con una cota fácil: la norma de Frobenius vale
    // sqrt(3) para la IDENTIDAD, y como esto se multiplica a lo largo de la
    // cadena de huesos, un esqueleto de diez niveles saldría 240 veces más
    // grande de lo que es y no se culearía nunca.
    static float operatorNorm3(const glm::mat4& m)
    {
        const glm::mat3 r  = glm::mat3(m);
        const glm::mat3 a  = glm::transpose(r) * r;   // simétrica y semidefinida positiva
        // Cualquier elemento de la diagonal es una cota INFERIOR del mayor
        // autovalor (cociente de Rayleigh sobre los ejes). Se usa de red: si la
        // forma cerrada se va por redondeo, el resultado sigue sin quedarse corto.
        float lambda = std::max(a[0][0], std::max(a[1][1], a[2][2]));

        const float p1 = a[1][0]*a[1][0] + a[2][0]*a[2][0] + a[2][1]*a[2][1];
        if (p1 > 0.0f)
        {
            const float q  = (a[0][0] + a[1][1] + a[2][2]) / 3.0f;
            const float p2 = (a[0][0]-q)*(a[0][0]-q) + (a[1][1]-q)*(a[1][1]-q)
                           + (a[2][2]-q)*(a[2][2]-q) + 2.0f*p1;
            const float p  = std::sqrt(p2 / 6.0f);
            if (p > 0.0f)
            {
                const glm::mat3 b   = (a - q * glm::mat3(1.0f)) * (1.0f / p);
                const float     det = glm::determinant(b);
                const float     phi = std::acos(std::clamp(det * 0.5f, -1.0f, 1.0f)) / 3.0f;
                lambda = std::max(lambda, q + 2.0f * p * std::cos(phi));
            }
        }
        return (lambda > 0.0f) ? std::sqrt(lambda) : 0.0f;
    }

    float Renderer::skinnedBoundRadius(const SkinnedMesh& mesh)
    {
        const Skeleton& skel      = mesh.skeleton;
        const int       boneCount = (int)skel.names.size();
        if (boneCount <= 0 || mesh.skinnedVertices.empty()) return 0.0f;
        if ((int)skel.parentIndex.size() < boneCount ||
            (int)skel.inverseBindPose.size() < boneCount) return 0.0f;

        // r[b]: radio de la nube de vértices que arrastra el hueso b, medido en
        // el espacio del PROPIO hueso — por eso el inverseBindPose. Es
        // invariante a la pose: skinning.comp le aplica encima la matriz de
        // mundo del hueso y nada más, así que la distancia al origen del hueso
        // sólo puede crecer por la escala, que se contabiliza aparte.
        std::vector<float> radio((size_t)boneCount, 0.0f);
        // Los pesos de un vértice deberían sumar 1, pero un FBX puede traerlos
        // sin normalizar y skinning.comp no los normaliza: sumar más de 1
        // alejaría el vértice más allá de la envolvente convexa de los huesos.
        float maxPeso = 1.0f;
        for (const SkinnedVertex& v : mesh.skinnedVertices)
        {
            float suma = 0.0f;
            for (int i = 0; i < 4; i++)
            {
                const float w = v.boneWeights[i];
                if (w <= 0.0f) continue;              // misma condición que skinning.comp
                const int b = v.boneIndices[i];
                if (b < 0 || b >= boneCount) continue; // índice basura: el shader ya leería fuera
                suma += w;
                const glm::vec3 p = glm::vec3(skel.inverseBindPose[b] *
                                              glm::vec4(glm::vec3(v.position), 1.0f));
                radio[(size_t)b] = std::max(radio[(size_t)b], glm::length(p));
            }
            maxPeso = std::max(maxPeso, suma);
        }

        // Cota del transform LOCAL de cada hueso, tomando el peor clip. Sólo se
        // miran los extremos de las keys: eso es lo que hace que la cota valga
        // también entre keyframes, sin muestrear poses.
        std::vector<float> maxTrans((size_t)boneCount, 0.0f);
        std::vector<float> maxEscala((size_t)boneCount, 0.0f);
        const size_t clipCount = mesh.animationClips.empty() ? 1u : mesh.animationClips.size();
        for (int b = 0; b < boneCount; b++)
        {
            // Local de bind pose, el default de un hueso del que el clip no dice
            // nada (mismo cálculo y mismo motivo que packSkinnedClips).
            const glm::mat4 globalBind = glm::inverse(skel.inverseBindPose[b]);
            const int       padre      = skel.parentIndex[b];
            const glm::mat4 bindLocal  = (padre < 0) ? globalBind
                                                     : skel.inverseBindPose[padre] * globalBind;
            // El mayor de los vectores columna NO vale como cota: si el FBX trae
            // shear las columnas no son ortogonales y se quedaría corto.
            const float bindTrans  = glm::length(glm::vec3(bindLocal[3]));
            const float bindEscala = operatorNorm3(bindLocal);

            for (size_t c = 0; c < clipCount; c++)
            {
                const BoneChannel* ch = nullptr;
                if (!mesh.animationClips.empty())
                    for (const BoneChannel& cc : mesh.animationClips[c].channels)
                        if (cc.boneIndex == b) { ch = &cc; break; }

                // Sin canal, o con canal pero sin ninguna key: bone_eval.comp
                // cae al bindLocal entero.
                const bool sinKeys = !ch || (ch->posKeys.empty() && ch->rotKeys.empty() &&
                                             ch->scaleKeys.empty());
                if (sinKeys)
                {
                    maxTrans[(size_t)b]  = std::max(maxTrans[(size_t)b],  bindTrans);
                    maxEscala[(size_t)b] = std::max(maxEscala[(size_t)b], bindEscala);
                    continue;
                }
                // Con keys de otro canal pero sin las suyas, bone_eval usa los
                // neutros del shader: posición 0 y escala 1, NO los del bind.
                float t = 0.0f;
                for (const BoneKeyframe& k : ch->posKeys) t = std::max(t, glm::length(k.value));
                float s = 1.0f;
                for (const BoneKeyframe& k : ch->scaleKeys)
                    s = std::max(s, std::max(std::abs(k.value.x),
                                  std::max(std::abs(k.value.y), std::abs(k.value.z))));
                maxTrans[(size_t)b]  = std::max(maxTrans[(size_t)b],  t);
                maxEscala[(size_t)b] = std::max(maxEscala[(size_t)b], s);
            }
        }

        // Propagación por la jerarquía. alcance[b] = distancia máxima del origen
        // del hueso al origen del modelo; cadena[b] = cota de la escala
        // acumulada desde la raíz. La traslación local de b la aplica la parte
        // lineal del PADRE, de ahí que use cadena[padre] y no cadena[b].
        //
        // Se apoya en que el padre va antes que el hijo, el mismo orden
        // topológico del que ya depende bone_hierarchy.comp.
        std::vector<float> alcance((size_t)boneCount, 0.0f);
        std::vector<float> cadena((size_t)boneCount, 1.0f);
        float R = 0.0f;
        for (int b = 0; b < boneCount; b++)
        {
            const int   padre       = skel.parentIndex[b];
            const bool  tienePadre  = (padre >= 0 && padre < b);
            const float alcancePadre = tienePadre ? alcance[(size_t)padre] : 0.0f;
            const float cadenaPadre  = tienePadre ? cadena[(size_t)padre]  : 1.0f;
            alcance[(size_t)b] = alcancePadre + cadenaPadre * maxTrans[(size_t)b];
            cadena[(size_t)b]  = cadenaPadre * maxEscala[(size_t)b];
            R = std::max(R, alcance[(size_t)b] + cadena[(size_t)b] * radio[(size_t)b]);
        }

        R *= maxPeso;
        // Un NaN o un infinito colados desde el modelo harían pasar el test de
        // culling de forma impredecible: mejor devolver "sin cota".
        return std::isfinite(R) ? R : 0.0f;
    }

    // Reparto de los cortes entre cascadas: mezcla del logarítmico (que da
    // resolución donde de verdad se ve, cerca) y el uniforme (que no deja la
    // última cascada cubriendo casi todo el mundo). lambda=0.75 tira hacia el
    // logarítmico, que es lo que se quiere con far grandes.
    static constexpr float kCascadeLambda = 0.75f;
    // Alcance máximo de las sombras. Un far de cámara de 5000 repartido entre 4
    // cascadas dejaría la primera cubriendo cientos de unidades: a 2048² eso son
    // sombras de bloques. Más allá de esta distancia no hay sombra, igual que
    // antes no la había fuera de la ortográfica de ±350.
    static constexpr float kShadowMaxDistance = 500.0f;
    // Margen por detrás del volumen de cada cascada, en la dirección de la luz.
    // Sin él, un objeto alto que queda fuera del frustum de la cámara pero cuya
    // sombra sí cae dentro no se dibujaría en el shadow map.
    static constexpr float kCasterMargin = 200.0f;

    void Renderer::computeCascades()
    {
        for (int i = 0; i < SHADOW_CASCADES; i++) m_cascadeMatrices[i] = glm::mat4(1.0f);
        m_cascadeSplits = glm::vec4(0.0f);
        if (m_lights.empty()) return;

        const FrameCamera fc = currentFrameCamera();

        // Esquinas del frustum, desproyectando el cubo NDC. z va de 0 a 1 y no
        // de -1 a 1 porque ese es el rango que Vulkan clipea: lo que se dibuja
        // de verdad está siempre entre esos dos planos.
        const glm::mat4 invViewProj = glm::inverse(fc.proj * fc.view);
        glm::vec3 cornerNear[4], cornerFar[4];
        const float ndcX[4] = { -1.0f,  1.0f,  1.0f, -1.0f };
        const float ndcY[4] = { -1.0f, -1.0f,  1.0f,  1.0f };
        for (int i = 0; i < 4; i++)
        {
            glm::vec4 pn = invViewProj * glm::vec4(ndcX[i], ndcY[i], 0.0f, 1.0f);
            glm::vec4 pf = invViewProj * glm::vec4(ndcX[i], ndcY[i], 1.0f, 1.0f);
            if (std::abs(pn.w) < 1e-8f || std::abs(pf.w) < 1e-8f) return;   // proyección degenerada
            cornerNear[i] = glm::vec3(pn) / pn.w;
            cornerFar[i]  = glm::vec3(pf) / pf.w;
        }

        // near/far REALES: la profundidad en view space de esos dos planos. No
        // se sacan de los coeficientes de fc.proj a propósito — el editor
        // construye su proyección con glm::perspective (z en [-1,1]) y el
        // CameraComponent con *RH_ZO, así que los mismos coeficientes
        // significan cosas distintas y la fórmula tendría que saber cuál está
        // activa. Los planos z=0 y z=1, en cambio, son los mismos en los dos
        // casos, y las 4 esquinas de cada uno están a profundidad constante.
        const float camNear = -(fc.view * glm::vec4(cornerNear[0], 1.0f)).z;
        const float camFar  = -(fc.view * glm::vec4(cornerFar[0],  1.0f)).z;
        if (!std::isfinite(camNear) || !std::isfinite(camFar) ||
            camNear <= 0.0f || camFar <= camNear)
        {
            return;
        }

        // Las esquinas ya están puestas con el far REAL (es el que define los
        // rayos del frustum); el reparto de cascadas usa el far recortado.
        const float shadowFar = std::min(camFar, kShadowMaxDistance);
        if (shadowFar <= camNear) return;

        // Luz direccional: la posición solo da la dirección, igual que antes
        // (lookAt desde la luz hacia el origen).
        const glm::vec3 lightPos = glm::vec3(m_lights[0].position);
        const float     lightLen = glm::length(lightPos);
        if (lightLen < 1e-6f) return;                       // luz en el origen: sin dirección
        const glm::vec3 lightDir = -lightPos / lightLen;    // de la luz hacia la escena
        const glm::vec3 up = std::abs(lightDir.y) > 0.99f ? glm::vec3(0.0f, 0.0f, 1.0f)
                                                          : glm::vec3(0.0f, 1.0f, 0.0f);
        const glm::mat4 lightRot    = glm::lookAt(glm::vec3(0.0f), lightDir, up);
        const glm::mat4 invLightRot = glm::inverse(lightRot);

        float prevDist = camNear;
        for (int c = 0; c < SHADOW_CASCADES; c++)
        {
            const float p         = (float)(c + 1) / (float)SHADOW_CASCADES;
            const float logSplit  = camNear * std::pow(shadowFar / camNear, p);
            const float uniSplit  = camNear + (shadowFar - camNear) * p;
            const float dist      = kCascadeLambda * logSplit + (1.0f - kCascadeLambda) * uniSplit;
            m_cascadeSplits[c]    = dist;

            // Interpolar entre las esquinas cercana y lejana es exacto: la
            // profundidad en view space varía linealmente a lo largo de ese
            // segmento. Los factores se calculan contra el far REAL porque es el
            // que sitúa cornerFar.
            const float tNear = (prevDist - camNear) / (camFar - camNear);
            const float tFar  = (dist     - camNear) / (camFar - camNear);

            glm::vec3 corners[8];
            for (int i = 0; i < 4; i++)
            {
                const glm::vec3 ray = cornerFar[i] - cornerNear[i];
                corners[i]     = cornerNear[i] + ray * tNear;
                corners[i + 4] = cornerNear[i] + ray * tFar;
            }

            // Esfera envolvente y no AABB: el radio no depende de hacia dónde
            // mire la cámara, así que girar en el sitio no cambia el tamaño del
            // volumen y las sombras no laten.
            glm::vec3 center(0.0f);
            for (const glm::vec3& v : corners) center += v;
            center /= 8.0f;
            float radius = 0.0f;
            for (const glm::vec3& v : corners) radius = std::max(radius, glm::length(v - center));
            // Cuantizar el radio evita que un cambio mínimo de la cámara mueva
            // el borde del volumen y con él todos los téxeles.
            radius = std::ceil(radius * 16.0f) / 16.0f;
            if (radius < 1e-4f) radius = 1e-4f;

            // Snap del centro a téxeles del shadow map, en el espacio de la luz.
            // Sin esto, avanzar la cámara arrastra el volumen de forma continua
            // y los bordes de sombra hierven.
            const float unitsPerTexel = (2.0f * radius) / (float)SHADOW_SIZE;
            glm::vec3 centerLS = glm::vec3(lightRot * glm::vec4(center, 1.0f));
            centerLS.x = std::floor(centerLS.x / unitsPerTexel) * unitsPerTexel;
            centerLS.y = std::floor(centerLS.y / unitsPerTexel) * unitsPerTexel;
            center = glm::vec3(invLightRot * glm::vec4(centerLS, 1.0f));

            const glm::mat4 lightView = glm::lookAt(center - lightDir * (radius + kCasterMargin),
                                                    center, up);
            glm::mat4 lightProj = glm::orthoRH_ZO(-radius, radius, -radius, radius,
                                                  0.0f, 2.0f * radius + kCasterMargin);
            lightProj[1][1] *= -1.0f;
            m_cascadeMatrices[c] = lightProj * lightView;

            prevDist = dist;
        }
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
            if (m_aaPassStamped[m_currentFrame])
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

        recordComputePass(m_commandBuffers[m_currentFrame]);
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
        recordFpCullPass(m_commandBuffers[m_currentFrame], fc.proj);

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
            // m_shadowPipelineLayout, que solo declara dos sets, y bindear con el
            // deja el set 2 sin definir.
            if (m_fpSets[m_currentFrame] != VK_NULL_HANDLE)
            {
                vkCmdBindDescriptorSets(m_commandBuffers[m_currentFrame], VK_PIPELINE_BIND_POINT_GRAPHICS,
                    m_pipelineLayout, 2, 1, &m_fpSets[m_currentFrame], 0, nullptr);
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
            const glm::mat4 proj = m_taaJitteredProj;

            // Skybox — fullscreen quad, depth LEQUAL sin escritura (al final del pass).
            // Omitido en wireframe: el fondo ya es negro sólido (clearValue por defecto).
            if (!isWireframeMode() && m_skybox.isInitialized()) {
                glm::mat4 rotView    = glm::mat4(glm::mat3(fc.view)); // sin traslación
                glm::mat4 invViewProj = glm::inverse(proj * rotView);
                m_skybox.draw(m_commandBuffers[m_currentFrame], invViewProj);
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
        recordSsrPass(m_commandBuffers[m_currentFrame], fc.proj);

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
                // Lectura de los timestamps de hace dos frames en este mismo slot: la
                // fence de m_currentFrame ya la esperó drawFrame, así que los
                // resultados están sin bloquear a nadie.
                if (m_timestampsSupported && m_bloomQueryPending[m_currentFrame])
                {
                    uint64_t stamps[2] = {};
                    if (vkGetQueryPoolResults(m_gpu.device(), m_bloomQueryPool, m_currentFrame * 2, 2,
                                              sizeof(stamps), stamps, sizeof(uint64_t),
                                              VK_QUERY_RESULT_64_BIT) == VK_SUCCESS)
                    {
                        m_bloomGpuMs = (float)((double)(stamps[1] - stamps[0]) * m_timestampPeriod * 1e-6);
                        if (++m_bloomMeasuredFrames == 300)
                        {
                            printf("bloom+composite: %.3f ms (%ux%u, %u mips)\n",
                                   m_bloomGpuMs, m_swapChainExtent.width, m_swapChainExtent.height,
                                   m_bloomMipCount);
                            fflush(stdout);
                        }
                    }
                }
                if (m_timestampsSupported)
                {
                    vkCmdResetQueryPool(cmd, m_bloomQueryPool, m_currentFrame * 2, 2);
                    vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, m_bloomQueryPool, m_currentFrame * 2);
                    m_bloomQueryPending[m_currentFrame] = true;
                }

                recordBloomPass(cmd);
            }
            else
            {
                // Apagado: ni un dispatch de la cadena, y sin timestamps que medir.
                // El slot deja de tener par pendiente para que al reencender no se
                // lea una medida de antes del apagón.
                m_bloomGpuMs = 0.0f;
                m_bloomQueryPending[m_currentFrame] = false;
                recordBloomClear(cmd);
            }

            VkRenderPassBeginInfo rpInfo{};
            rpInfo.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
            rpInfo.renderPass        = m_compositeRenderPass;
            // Con FXAA, SSAA o TAA la composicion (tonemap + contorno + gizmos)
            // va a la imagen intermedia y el pass de resolucion la lleva de ahi a
            // m_offscreenImage. En None y en MSAA escribe directamente en
            // m_offscreenImage, exactamente como antes de esta feature: mismo
            // render pass, mismos comandos.
            rpInfo.framebuffer       = needsAaIntermediate() ? m_aaSrcFramebuffer[m_currentFrame]
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
            const float intensity = (bloomEnabled() && m_bloomMipCount > 0) ? m_bloomIntensity : 0.0f;
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

            // UI de juego, lo ultimo del pass: va encima de la escena y de los
            // gizmos, y por debajo de la UI del editor (que se graba en el pass
            // del swapchain). Con el canvas vacio no se graba ni un comando.
            m_uiCanvas.buildDrawData(m_renderExtent.width, m_renderExtent.height, m_uiDrawData);
            m_uiBatch.record(m_gpu, cmd, m_uiDrawData, m_renderExtent, m_currentFrame);

            vkCmdEndRenderPass(cmd);

            // Solo con el bloom encendido: el par se abre arriba bajo la misma
            // condición, y escribir aquí sin haber reseteado dejaría la query sucia.
            if (m_timestampsSupported && bloomEnabled())
                vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, m_bloomQueryPool, m_currentFrame * 2 + 1);

            // Anti-aliasing: lo ultimo de la cadena de post, sobre color LDR ya
            // tonemapeado y con el contorno de seleccion y los gizmos ya dibujados
            // (asi que tambien se les suavizan los bordes, que es lo deseado: sus
            // lineas y el casco invertido son lo mas escalonado de la pantalla).
            recordAaPass(cmd);

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
        VkDescriptorSetLayout setLayouts[] = { m_descriptorSetLayout, m_instanceDescLayout, m_fpDescLayout };

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
            vkCreateBuffer(m_gpu.device(), &bufferInfo, nullptr, &m_uniformBuffers[i]);

            VkMemoryRequirements memoryRequirements;
            vkGetBufferMemoryRequirements(m_gpu.device(), m_uniformBuffers[i], &memoryRequirements);

            VkMemoryAllocateInfo allocInfo{};
            allocInfo.sType             = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            allocInfo.allocationSize    = memoryRequirements.size;
            allocInfo.memoryTypeIndex   = m_gpu.findMemoryType(memoryRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            vkAllocateMemory(m_gpu.device(), &allocInfo, NULL, &m_uniformBuffersMemory[i]);
            vkBindBufferMemory(m_gpu.device(), m_uniformBuffers[i], m_uniformBuffersMemory[i], 0);

            // Mapeo persistente — nunca llamamos unmap
            vkMapMemory(m_gpu.device(), m_uniformBuffersMemory[i], 0, size, 0, &m_uniformBuffersMapped[i]);

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

        // Pool propio y no m_descriptorPool: ese se dimensiona por objeto y solo
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
        uint32_t n = (uint32_t)((m_objects.size() + 128) * MAX_FRAMES);
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

        if(vkCreateDescriptorPool(m_gpu.device(), &poolInfo, nullptr, &m_descriptorPool) != VK_SUCCESS)
            throw std::runtime_error("failed to create descriptor pool!");
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
        VkDescriptorSetLayout layouts[MAX_FRAMES] = { m_descriptorSetLayout, m_descriptorSetLayout };

        VkDescriptorSetAllocateInfo allocInfo{};
        allocInfo.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocInfo.descriptorPool     = m_descriptorPool;
        allocInfo.descriptorSetCount = MAX_FRAMES;
        allocInfo.pSetLayouts        = layouts;

        if (vkAllocateDescriptorSets(m_gpu.device(), &allocInfo, obj.descriptorSets) != VK_SUCCESS)
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
            shadowInfo.imageView   = m_shadowView;
            shadowInfo.sampler     = m_shadowSampler;

            VkDescriptorImageInfo ormInfo{};
            ormInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            ormInfo.imageView   = obj.ormView;
            ormInfo.sampler     = obj.ormSampler;

            // IBL: los mismos dos cubemaps para todos los objetos. Existen desde
            // init(), asi que estos writes valen aunque no haya skybox.
            VkDescriptorImageInfo irradianceInfo{};
            irradianceInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            irradianceInfo.imageView   = m_iblIrradianceView;
            irradianceInfo.sampler     = m_iblSampler;

            VkDescriptorImageInfo prefilterInfo{};
            prefilterInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            prefilterInfo.imageView   = m_iblPrefilterView;
            prefilterInfo.sampler     = m_iblSampler;

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
        if (m_ssaoBlurView[frameIndex] == VK_NULL_HANDLE) return;

        VkDescriptorImageInfo info{};
        // GENERAL y no SHADER_READ_ONLY: la misma imagen es storage image del
        // compute y textura del pass de escena, igual que la cadena del bloom.
        info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        info.imageView   = m_ssaoBlurView[frameIndex];
        info.sampler     = m_ssaoSampler;

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
            for (int i = 0; i < MAX_FRAMES; i++) m_ssaoClearPending[i] = true;
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
        m_taaPrevViewProj  = m_taaCurrViewProj;
        m_taaCurrViewProj  = fc.proj * fc.view;
        m_taaJitteredProj  = fc.proj;
        if (m_aaActiveMode == AaMode::Taa)
        {
            // Halton(2,3) desplazado a [-0.5, 0.5] píxeles. 16 posiciones antes
            // de repetir: suficiente para que el promedio sea estable y corto
            // para que el ciclo no se note al parar la cámara.
            m_taaJitter.x = (halton(m_taaJitterIndex + 1, 2) - 0.5f) * m_taaJitterScale;
            m_taaJitter.y = (halton(m_taaJitterIndex + 1, 3) - 0.5f) * m_taaJitterScale;
            m_taaJitterIndex = (m_taaJitterIndex + 1) % 16;

            // Desplazamiento en clip space: el ancho completo del clip es 2, de
            // ahí el factor. Se aplica sobre la columna de la Z para que el
            // desplazamiento sea constante en pantalla a cualquier profundidad.
            m_taaJitteredProj[2][0] += 2.0f * m_taaJitter.x / (float)m_renderExtent.width;
            m_taaJitteredProj[2][1] += 2.0f * m_taaJitter.y / (float)m_renderExtent.height;
        }

        UniformBufferObject ubo{};
        ubo.view = fc.view;
        // Con TAA sale jittereada; en cualquier otro modo es fc.proj tal cual.
        ubo.proj = m_taaJitteredProj;
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
            ubo.lightSpaceMatrix[i] = m_cascadeMatrices[i];
        }
        ubo.cascadeSplits = m_cascadeSplits;

        memcpy(m_uniformBuffersMapped[frameIndex], &ubo, sizeof(ubo));
        // El bake de una reflection probe parte de este mismo buffer (luces y
        // matrices de cascada del frame); hasta que se escribe una vez es basura.
        m_uboWritten[frameIndex] = true;

        // ── Forward+: bloque de parametros y lista de luces ──────────────────
        // Se escribe SIEMPRE, tambien en Off: pbr.frag lee fp.mode de aqui para
        // decidir por que rama va, y con 0 no toca ni un buffer mas.
        if (m_fpParamsMapped[frameIndex])
        {
            uint32_t gx = 0, gy = 0, gz = 0, ts = 0;
            fpGridDims(m_fpActiveMode, gx, gy, gz, ts);

            // zNear/zFar salen de la propia proyeccion (RH_ZO): p22 = f/(n-f) y
            // p32 = f*n/(n-f), asi que n = p32/p22 y f = p32/(p22+1). Es la unica
            // forma de que la rejilla siga a la camara del CameraComponent en Play
            // sin duplicar aqui los planos de la camara del editor.
            const float p22 = fc.proj[2][2];
            const float p32 = fc.proj[3][2];
            const float zNear = (p22 != 0.0f) ? p32 / p22 : 0.1f;
            const float zFar  = (p22 != -1.0f) ? p32 / (p22 + 1.0f) : 1000.0f;

            const uint32_t count = (uint32_t)std::min<size_t>(m_lights.size(), kFpMaxLights);

            FpParamsGpu fp{};
            fp.mode       = (uint32_t)m_fpActiveMode;
            fp.gridX      = gx;
            fp.gridY      = gy;
            fp.gridZ      = gz;
            fp.tileSize   = ts;
            fp.maxPerCell = kFpMaxPerCell;
            fp.numLights  = count;
            fp.zNear      = zNear;
            fp.zFar       = zFar;
            // Inverso del reparto logaritmico de light_cull_clustered.comp:
            // slice = log2(z)*scale + bias.
            const float logRatio = std::log2(std::max(zFar / zNear, 1.0001f));
            fp.sliceScale = (float)gz / logRatio;
            fp.sliceBias  = -std::log2(zNear) * fp.sliceScale;
            memcpy(m_fpParamsMapped[frameIndex], &fp, sizeof(fp));

            if (m_fpLightMapped[frameIndex] && count > 0)
            {
                FpLightGpu* dst = (FpLightGpu*)m_fpLightMapped[frameIndex];
                for (uint32_t i = 0; i < count; i++)
                {
                    // El radio es el unico dato que Light no lleva: por luz si el
                    // usuario lo ha dado, y si no el global. En el UBO no cabe sin
                    // mover el layout std140 que declaran 5 shaders.
                    const float radius = (i < m_lightRadii.size()) ? m_lightRadii[i] : m_fpLightRadius;
                    const glm::vec3 wp = glm::vec3(m_lights[i].position);
                    // La misma luz en view space, para que el culling no necesite
                    // la matriz de vista ni la recalcule por celda.
                    const glm::vec3 vp = glm::vec3(fc.view * glm::vec4(wp, 1.0f));
                    dst[i].posRadius = glm::vec4(wp, radius);
                    dst[i].color     = m_lights[i].color;
                    dst[i].viewPosR  = glm::vec4(vp, radius);
                    dst[i].direction = m_lights[i].direction;
                    dst[i].params    = m_lights[i].params;
                }
            }
        }
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
        // rebuild agotaba m_descriptorPool en vez de reciclar sus sets. No hace
        // falta anularlos: obj es siempre la copia que la cache sacó de la
        // tabla, y el slot original ya quedó vacío.
        if (obj.descriptorSets[0] != VK_NULL_HANDLE)
            vkFreeDescriptorSets(m_gpu.device(), m_descriptorPool, MAX_FRAMES, obj.descriptorSets);
    }

    void Renderer::createShadowResources()
    {
        // 1. Imagen depth para shadow map: un texture array con una capa por
        // cascada. No usa m_res.createImage porque esa fija arrayLayers a 1 y
        // la firma la comparten todas las texturas del motor.
        VkImageCreateInfo imageInfo{};
        imageInfo.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType     = VK_IMAGE_TYPE_2D;
        imageInfo.format        = VK_FORMAT_D32_SFLOAT;
        imageInfo.extent        = { SHADOW_SIZE, SHADOW_SIZE, 1 };
        imageInfo.mipLevels     = 1;
        imageInfo.arrayLayers   = SHADOW_CASCADES;
        imageInfo.samples       = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling        = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage         = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        imageInfo.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkCreateImage(m_gpu.device(), &imageInfo, nullptr, &m_shadowImage) != VK_SUCCESS)
        {
            throw std::runtime_error("failed to create shadow image!");
        }

        VkMemoryRequirements memReq;
        vkGetImageMemoryRequirements(m_gpu.device(), m_shadowImage, &memReq);
        VkMemoryAllocateInfo memAlloc{};
        memAlloc.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        memAlloc.allocationSize  = memReq.size;
        memAlloc.memoryTypeIndex = m_gpu.findMemoryType(memReq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (vkAllocateMemory(m_gpu.device(), &memAlloc, nullptr, &m_shadowMemory) != VK_SUCCESS)
        {
            throw std::runtime_error("failed to allocate shadow image memory!");
        }
        vkBindImageMemory(m_gpu.device(), m_shadowImage, m_shadowMemory, 0);

        // 2. Image views: una del array entero para muestrear, y una por capa
        // para colgarle un framebuffer.
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType                          = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image                          = m_shadowImage;
        viewInfo.viewType                       = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        viewInfo.format                         = VK_FORMAT_D32_SFLOAT;
        viewInfo.subresourceRange.aspectMask    = VK_IMAGE_ASPECT_DEPTH_BIT;
        viewInfo.subresourceRange.layerCount    = SHADOW_CASCADES;
        viewInfo.subresourceRange.levelCount    = 1;
        if(vkCreateImageView(m_gpu.device(), &viewInfo, nullptr, &m_shadowView) != VK_SUCCESS)
        {
            throw std::runtime_error("failed to create shadow image view!");
        }

        for (uint32_t c = 0; c < SHADOW_CASCADES; c++)
        {
            VkImageViewCreateInfo layerInfo = viewInfo;
            layerInfo.viewType                        = VK_IMAGE_VIEW_TYPE_2D;
            layerInfo.subresourceRange.baseArrayLayer = c;
            layerInfo.subresourceRange.layerCount     = 1;
            if (vkCreateImageView(m_gpu.device(), &layerInfo, nullptr, &m_shadowLayerViews[c]) != VK_SUCCESS)
            {
                throw std::runtime_error("failed to create shadow layer view!");
            }
        }

        // 3. Sampler de comparación (PCF listo)
        VkSamplerCreateInfo samplerInfo{};
        samplerInfo.sType                   = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        samplerInfo.magFilter               = VK_FILTER_LINEAR;
        samplerInfo.minFilter               = VK_FILTER_LINEAR;
        samplerInfo.addressModeU            = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        samplerInfo.addressModeV            = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        samplerInfo.addressModeW            = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        samplerInfo.borderColor             = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
        samplerInfo.compareEnable           = VK_TRUE;
        samplerInfo.compareOp               = VK_COMPARE_OP_LESS_OR_EQUAL;
        samplerInfo.mipmapMode              = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        if(vkCreateSampler(m_gpu.device(), &samplerInfo, nullptr, &m_shadowSampler) != VK_SUCCESS)
        {
            throw std::runtime_error("failed to create shadow sampler!");
        }

        // 4. Render pass depth-only
        VkAttachmentDescription depthAttachment{};
        depthAttachment.format         = VK_FORMAT_D32_SFLOAT;
        depthAttachment.samples        = VK_SAMPLE_COUNT_1_BIT;
        depthAttachment.loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depthAttachment.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
        depthAttachment.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        depthAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depthAttachment.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
        depthAttachment.finalLayout    = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
        
        VkAttachmentReference depthAttachmentRef{0, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};

        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.pDepthStencilAttachment = &depthAttachmentRef;

        VkSubpassDependency dependencies[2]{};
        dependencies[0].srcSubpass      = VK_SUBPASS_EXTERNAL;
        dependencies[0].dstSubpass      = 0;
        dependencies[0].srcStageMask    = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        dependencies[0].dstStageMask    = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        dependencies[0].srcAccessMask   = VK_ACCESS_SHADER_READ_BIT;
        dependencies[0].dstAccessMask   = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        dependencies[0].dependencyFlags  = VK_DEPENDENCY_BY_REGION_BIT;
        dependencies[1].srcSubpass      = 0;
        dependencies[1].dstSubpass      = VK_SUBPASS_EXTERNAL;
        dependencies[1].srcStageMask    = VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        dependencies[1].dstStageMask    = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        dependencies[1].srcAccessMask   = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        dependencies[1].dstAccessMask   = VK_ACCESS_SHADER_READ_BIT;
        dependencies[1].dependencyFlags  = VK_DEPENDENCY_BY_REGION_BIT;

        VkRenderPassCreateInfo renderPassInfo{};
        renderPassInfo.sType            = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        renderPassInfo.attachmentCount   = 1;
        renderPassInfo.pAttachments      = &depthAttachment;
        renderPassInfo.subpassCount      = 1;
        renderPassInfo.pSubpasses        = &subpass;
        renderPassInfo.dependencyCount   = 2;
        renderPassInfo.pDependencies     = dependencies;
        if(vkCreateRenderPass(m_gpu.device(), &renderPassInfo, nullptr, &m_shadowRenderPass) != VK_SUCCESS)
        {
            throw std::runtime_error("failed to create shadow render pass!");
        }

         // 5. Framebuffers: uno por cascada, cada uno sobre su capa. Todos
         // comparten el render pass (el formato del attachment es el mismo).
         for (uint32_t c = 0; c < SHADOW_CASCADES; c++)
         {
            VkFramebufferCreateInfo fbInfo{};
            fbInfo.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            fbInfo.renderPass      = m_shadowRenderPass;
            fbInfo.attachmentCount = 1;
            fbInfo.pAttachments    = &m_shadowLayerViews[c];
            fbInfo.width           = SHADOW_SIZE;
            fbInfo.height          = SHADOW_SIZE;
            fbInfo.layers          = 1;
            if (vkCreateFramebuffer(m_gpu.device(), &fbInfo, nullptr, &m_shadowFramebuffers[c]) != VK_SUCCESS)
            {
                throw std::runtime_error("failed to create shadow framebuffer!");
            }
         }

        // 6. Pipeline (vertex-only, sin color attachments)
        auto vertCode = loadShaderFile("shaders/shadow.vert.spv");
        VkShaderModule vertModule = createShaderModule(vertCode);

        VkPipelineShaderStageCreateInfo vertStage{};
        vertStage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        vertStage.stage  = VK_SHADER_STAGE_VERTEX_BIT;
        vertStage.module = vertModule;
        vertStage.pName  = "main";

        VkVertexInputBindingDescription bindingDesc{};
        bindingDesc.binding   = 0;
        bindingDesc.stride    = sizeof(Vertex);
        bindingDesc.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

        VkVertexInputAttributeDescription attrDesc{};
        attrDesc.binding  = 0;
        attrDesc.location = 0;
        attrDesc.format   = VK_FORMAT_R32G32B32_SFLOAT;
        attrDesc.offset   = offsetof(Vertex, pos);

        VkPipelineVertexInputStateCreateInfo vertexInput{};
        vertexInput.sType                           = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vertexInput.vertexBindingDescriptionCount   = 1;
        vertexInput.pVertexBindingDescriptions      = &bindingDesc;
        vertexInput.vertexAttributeDescriptionCount = 1;
        vertexInput.pVertexAttributeDescriptions    = &attrDesc;

        VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
        inputAssembly.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

        VkPipelineViewportStateCreateInfo viewportState{};
        viewportState.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        viewportState.viewportCount = 1;
        viewportState.scissorCount  = 1;

        VkPipelineRasterizationStateCreateInfo rasterizer{};
        rasterizer.sType                   = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rasterizer.polygonMode             = VK_POLYGON_MODE_FILL;
        rasterizer.cullMode                = VK_CULL_MODE_NONE;
        rasterizer.frontFace               = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rasterizer.lineWidth               = 1.0f;
        rasterizer.depthBiasEnable         = VK_TRUE;
        rasterizer.depthBiasConstantFactor = 1.25f;
        rasterizer.depthBiasSlopeFactor    = 1.75f;

        VkPipelineMultisampleStateCreateInfo multisampling{};
        multisampling.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        VkPipelineDepthStencilStateCreateInfo depthStencil{};
        depthStencil.sType            = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        depthStencil.depthTestEnable  = VK_TRUE;
        depthStencil.depthWriteEnable = VK_TRUE;
        depthStencil.depthCompareOp   = VK_COMPARE_OP_LESS_OR_EQUAL;

        VkPipelineColorBlendStateCreateInfo colorBlend{};
        colorBlend.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        colorBlend.attachmentCount = 0; // sin color attachments

        VkDynamicState dynStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo dynamicState{};
        dynamicState.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dynamicState.dynamicStateCount = 2;
        dynamicState.pDynamicStates    = dynStates;

        // El model matrix NO va por push constant: shadow.vert lo saca del SSBO
        // de instancias (set 1) por gl_InstanceIndex, igual que triangle.vert.
        // El único push constant es el índice de cascada, que dice cuál de las
        // matrices del UBO usar. Este layout es propio del pass de sombras y no
        // lo comparte ningún otro pipeline, así que el rango de PushData que
        // usan triangle/pbr/outline no se toca.
        VkPushConstantRange pcr{};
        pcr.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        pcr.offset     = 0;
        pcr.size       = sizeof(uint32_t);

        VkDescriptorSetLayout setLayouts[] = { m_descriptorSetLayout, m_instanceDescLayout };

        VkPipelineLayoutCreateInfo layoutInfo{};
        layoutInfo.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layoutInfo.setLayoutCount         = 2;
        layoutInfo.pSetLayouts            = setLayouts;
        layoutInfo.pushConstantRangeCount = 1;
        layoutInfo.pPushConstantRanges    = &pcr;
        if (vkCreatePipelineLayout(m_gpu.device(), &layoutInfo, nullptr, &m_shadowPipelineLayout) != VK_SUCCESS)
        {
            throw std::runtime_error("failed to create shadow pipeline layout!");
        }            

        VkGraphicsPipelineCreateInfo pipelineInfo{};
        pipelineInfo.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        pipelineInfo.stageCount          = 1;
        pipelineInfo.pStages             = &vertStage;
        pipelineInfo.pVertexInputState   = &vertexInput;
        pipelineInfo.pInputAssemblyState = &inputAssembly;
        pipelineInfo.pViewportState      = &viewportState;
        pipelineInfo.pRasterizationState = &rasterizer;
        pipelineInfo.pMultisampleState   = &multisampling;
        pipelineInfo.pDepthStencilState  = &depthStencil;
        pipelineInfo.pColorBlendState    = &colorBlend;
        pipelineInfo.pDynamicState       = &dynamicState;
        pipelineInfo.layout              = m_shadowPipelineLayout;
        pipelineInfo.renderPass          = m_shadowRenderPass;
        if (vkCreateGraphicsPipelines(m_gpu.device(), VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_shadowPipeline) != VK_SUCCESS)
        {
            throw std::runtime_error("failed to create shadow pipeline!");
        }

        // Variante para las mallas skinned. Todo el estado se copia del de
        // arriba (mismo bias, mismo depth, mismas cascadas, mismo layout), así
        // que la sombra de los estáticos no cambia. Lo único distinto es el
        // vertex input.
        //
        // stride 80, no sizeof(SkinnedVertex): ese es el vértice de ENTRADA del
        // compute (7×vec4, con índices y pesos de hueso). Lo que se dibuja aquí
        // es su SALIDA, el OutputVertex de skinning.comp, que son 5×vec4 y lleva
        // la posición en el primero. Es el mismo stride que declara el pipeline
        // skinned del pass principal.
        VkVertexInputBindingDescription skinnedBinding{};
        skinnedBinding.binding   = 0;
        skinnedBinding.stride    = 5 * (uint32_t)sizeof(glm::vec4);  // 80 bytes
        skinnedBinding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

        // pos es un vec4 (std430 del compute); shadow.vert solo declara vec3, y
        // leer 3 de los 4 floats es legal.
        VkVertexInputAttributeDescription skinnedAttr{};
        skinnedAttr.binding  = 0;
        skinnedAttr.location = 0;
        skinnedAttr.format   = VK_FORMAT_R32G32B32_SFLOAT;
        skinnedAttr.offset   = 0;

        VkPipelineVertexInputStateCreateInfo skinnedVertexInput = vertexInput;
        skinnedVertexInput.pVertexBindingDescriptions   = &skinnedBinding;
        skinnedVertexInput.pVertexAttributeDescriptions = &skinnedAttr;

        VkGraphicsPipelineCreateInfo skinnedPipelineInfo = pipelineInfo;
        skinnedPipelineInfo.pVertexInputState = &skinnedVertexInput;
        if (vkCreateGraphicsPipelines(m_gpu.device(), VK_NULL_HANDLE, 1, &skinnedPipelineInfo, nullptr, &m_shadowSkinnedPipeline) != VK_SUCCESS)
        {
            throw std::runtime_error("failed to create skinned shadow pipeline!");
        }

        vkDestroyShaderModule(m_gpu.device(), vertModule, nullptr);
    }

    void Renderer::recordShadowPass(VkCommandBuffer cmd)
    {
        VkClearValue clearDepth{};
        clearDepth.depthStencil = { 1.0f, 0 };

        // Sin luces no hay matrices que extraer (computeCascades deja la
        // identidad, cuyo frustum es el cubo unidad y culearía casi todo). Aun
        // así hay que abrir los N render pass: son los que limpian las capas y
        // las dejan en DEPTH_STENCIL_READ_ONLY_OPTIMAL, que es el layout que
        // declaran los descriptor sets. Lo que se salta es la geometría, que
        // nadie va a muestrear (numLights = 0 apaga el shadow en el shader).
        const bool drawCasters = !m_lights.empty();

        VkViewport vp {0.0f, 0.0f, (float)SHADOW_SIZE, (float)SHADOW_SIZE, 0.0f, 1.0f};
        VkRect2D sc {{0,0}, {SHADOW_SIZE, SHADOW_SIZE}};

        for (uint32_t cascade = 0; cascade < SHADOW_CASCADES; cascade++)
        {
            VkRenderPassBeginInfo renderPassInfo{};
            renderPassInfo.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
            renderPassInfo.renderPass        = m_shadowRenderPass;
            renderPassInfo.framebuffer       = m_shadowFramebuffers[cascade];
            renderPassInfo.renderArea.extent = { SHADOW_SIZE, SHADOW_SIZE };
            renderPassInfo.clearValueCount   = 1;
            renderPassInfo.pClearValues      = &clearDepth;

            vkCmdBeginRenderPass(cmd, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);

            if (!drawCasters)
            {
                vkCmdEndRenderPass(cmd);
                continue;
            }

            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_shadowPipeline);
            vkCmdSetViewport(cmd, 0, 1, &vp);
            vkCmdSetScissor(cmd, 0, 1, &sc);
            vkCmdPushConstants(cmd, m_shadowPipelineLayout, VK_SHADER_STAGE_VERTEX_BIT,
                               0, sizeof(uint32_t), &cascade);

            // Culling por el frustum de ESTA cascada, no por el de la cámara ni
            // por el de la cascada mayor: un objeto que la cámara no ve puede
            // seguir proyectando sombra sobre lo que sí se ve, y un objeto que
            // cae en la cascada lejana no pinta nada en el mapa de la cercana.
            const Frustum lightFrustum = frustumFromViewProj(m_cascadeMatrices[cascade]);

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

            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_shadowPipelineLayout,
                1, 1, &m_instanceDescSets[m_currentFrame], 0, nullptr);

            for (const InstanceBatch& batch : m_instanceBatches)
            {
                const SharedGpuMesh* gpu = m_sharedMeshes.get(batch.sharedIndex);
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_shadowPipelineLayout, 0, 1, &gpu->descriptorSets[m_currentFrame], 0, nullptr);

                VkBuffer vb[] = { gpu->vertexBuffer };
                VkDeviceSize offsets[] = { 0 };
                vkCmdBindVertexBuffers(cmd, 0, 1, vb, offsets);
                vkCmdBindIndexBuffer(cmd, gpu->indexBuffer, 0, VK_INDEX_TYPE_UINT32);
                vkCmdDrawIndexed(cmd, gpu->indexCount, batch.instanceCount, 0, 0, batch.firstInstance);
            }

            // Skinned. recordComputePass corre justo antes en este mismo command
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
                    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_shadowSkinnedPipeline);
                    // El layout es el mismo, así que el push constant de la
                    // cascada sobrevive al cambio de pipeline; se reescribe por
                    // no depender de esa compatibilidad.
                    vkCmdPushConstants(cmd, m_shadowPipelineLayout, VK_SHADER_STAGE_VERTEX_BIT,
                                       0, sizeof(uint32_t), &cascade);
                    skinnedBound = true;
                }

                // Set 0 solo por el UBO de la cascada (binding 0): shadow.vert no
                // muestrea nada, así que cualquier descriptor set del material
                // sirve mientras sea del layout que declara el pipeline.
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_shadowPipelineLayout,
                    0, 1, &sobj.matGfx[0].descSets[m_currentFrame], 0, nullptr);

                VkBuffer svb[] = { sobj.outputVertexBuffer };
                VkDeviceSize soffsets[] = { 0 };
                vkCmdBindVertexBuffers(cmd, 0, 1, svb, soffsets);
                vkCmdBindIndexBuffer(cmd, sobj.indexBuffer, 0, VK_INDEX_TYPE_UINT32);
                for (const auto& sm : sobj.subMeshes)
                    vkCmdDrawIndexed(cmd, sm.indexCount, 1, sm.indexStart, 0, instanceIndex);
            }

            vkCmdEndRenderPass(cmd);
        }
    }

    // ── IBL ─────────────────────────────────────────────────────────────────
    // Formato de los dos cubemaps. R16G16B16A16_SFLOAT tiene soporte OBLIGATORIO
    // como storage image en Vulkan, asi que no hace falta consultar
    // vkGetPhysicalDeviceFormatProperties. Con 8 bits el especular prefiltrado
    // se bandearia en las zonas de gradiente suave.
    static constexpr VkFormat kIblFormat = VK_FORMAT_R16G16B16A16_SFLOAT;

    void Renderer::createIblResources()
    {
        // 1. Las dos imagenes. No usan m_res.createImage: esa fija arrayLayers y
        // mipLevels a 1, y aqui hacen falta 6 capas (y mips en el prefiltrado).
        auto makeCube = [&](uint32_t size, uint32_t mips, VkImage& image, VkDeviceMemory& memory)
        {
            VkImageCreateInfo ci{};
            ci.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
            ci.flags         = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
            ci.imageType     = VK_IMAGE_TYPE_2D;
            ci.format        = kIblFormat;
            ci.extent        = { size, size, 1 };
            ci.mipLevels     = mips;
            ci.arrayLayers   = 6;
            ci.samples       = VK_SAMPLE_COUNT_1_BIT;
            ci.tiling        = VK_IMAGE_TILING_OPTIMAL;
            // TRANSFER_DST es pa el clear neutro de mas abajo, no pa una copia.
            ci.usage         = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT
                             | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
            ci.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
            ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            if (vkCreateImage(m_gpu.device(), &ci, nullptr, &image) != VK_SUCCESS)
                throw std::runtime_error("failed to create IBL cubemap image!");

            VkMemoryRequirements memReq;
            vkGetImageMemoryRequirements(m_gpu.device(), image, &memReq);
            VkMemoryAllocateInfo memAlloc{};
            memAlloc.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            memAlloc.allocationSize  = memReq.size;
            memAlloc.memoryTypeIndex = m_gpu.findMemoryType(memReq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            if (vkAllocateMemory(m_gpu.device(), &memAlloc, nullptr, &memory) != VK_SUCCESS)
                throw std::runtime_error("failed to allocate IBL cubemap memory!");
            vkBindImageMemory(m_gpu.device(), image, memory, 0);
        };

        makeCube(IBL_IRRADIANCE_SIZE, 1,                  m_iblIrradianceImage, m_iblIrradianceMemory);
        makeCube(IBL_PREFILTER_SIZE,  IBL_PREFILTER_MIPS, m_iblPrefilterImage,  m_iblPrefilterMemory);

        // 2. Vistas. La CUBE es la que va en los descriptor sets de los objetos;
        // las 2D_ARRAY solo existen pa que el compute las escriba como storage
        // image, una por nivel de mip porque imageStore no elige nivel.
        auto makeView = [&](VkImage image, VkImageViewType type, uint32_t baseMip, uint32_t mipCount, VkImageView& view)
        {
            VkImageViewCreateInfo vi{};
            vi.sType                           = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            vi.image                           = image;
            vi.viewType                        = type;
            vi.format                          = kIblFormat;
            vi.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
            vi.subresourceRange.baseMipLevel   = baseMip;
            vi.subresourceRange.levelCount     = mipCount;
            vi.subresourceRange.baseArrayLayer = 0;
            vi.subresourceRange.layerCount     = 6;
            if (vkCreateImageView(m_gpu.device(), &vi, nullptr, &view) != VK_SUCCESS)
                throw std::runtime_error("failed to create IBL image view!");
        };

        makeView(m_iblIrradianceImage, VK_IMAGE_VIEW_TYPE_CUBE,     0, 1, m_iblIrradianceView);
        makeView(m_iblIrradianceImage, VK_IMAGE_VIEW_TYPE_2D_ARRAY, 0, 1, m_iblIrradianceStore);
        makeView(m_iblPrefilterImage,  VK_IMAGE_VIEW_TYPE_CUBE,     0, IBL_PREFILTER_MIPS, m_iblPrefilterView);
        for (uint32_t m = 0; m < IBL_PREFILTER_MIPS; m++)
            makeView(m_iblPrefilterImage, VK_IMAGE_VIEW_TYPE_2D_ARRAY, m, 1, m_iblPrefilterStore[m]);

        // 3. Sampler comun. maxLod cubre los mips del prefiltrado; la vista de
        // irradiancia solo tiene un nivel, asi que ahi el LOD se recorta solo.
        VkSamplerCreateInfo si{};
        si.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        si.magFilter    = VK_FILTER_LINEAR;
        si.minFilter    = VK_FILTER_LINEAR;
        si.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.maxLod       = (float)IBL_PREFILTER_MIPS;
        if (vkCreateSampler(m_gpu.device(), &si, nullptr, &m_iblSampler) != VK_SUCCESS)
            throw std::runtime_error("failed to create IBL sampler!");

        // 4. Contenido neutro. Es lo que se ve si nunca se llama a initSkybox (o
        // si el cubemap no carga): el mismo ambiente plano de antes, en vez de
        // un descriptor apuntando a basura.
        {
            VkCommandBuffer cmd = m_gpu.beginOneTimeCommands();

            VkImageMemoryBarrier b{};
            b.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            b.subresourceRange.layerCount = 6;

            auto clearTo = [&](VkImage image, uint32_t mips, const VkClearColorValue& color)
            {
                b.image                       = image;
                b.subresourceRange.levelCount = mips;

                b.oldLayout     = VK_IMAGE_LAYOUT_UNDEFINED;
                b.newLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                b.srcAccessMask = 0;
                b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                     0, 0, nullptr, 0, nullptr, 1, &b);

                vkCmdClearColorImage(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                     &color, 1, &b.subresourceRange);

                b.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                b.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
                vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                     0, 0, nullptr, 0, nullptr, 1, &b);
            };

            // Los mismos numeros que tenia el ambiente hemisferico de pbr.frag:
            // la media de cielo y suelo pal difuso, el cielo pal especular.
            const VkClearColorValue irradianceNeutral{{ 0.075f, 0.080f, 0.090f, 1.0f }};
            const VkClearColorValue prefilterNeutral {{ 0.100f, 0.120f, 0.150f, 1.0f }};
            clearTo(m_iblIrradianceImage, 1,                  irradianceNeutral);
            clearTo(m_iblPrefilterImage,  IBL_PREFILTER_MIPS, prefilterNeutral);

            m_gpu.endOneTimeCommands(cmd);
        }

        // 5. Descriptor set layout, pool y pipelines de la precomputacion. Layout
        // propio y no el de createComputePipelines: ese son 8 storage buffers.
        VkDescriptorSetLayoutBinding iblBindings[2]{};
        iblBindings[0].binding         = 0;
        iblBindings[0].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        iblBindings[0].descriptorCount = 1;
        iblBindings[0].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
        iblBindings[1].binding         = 1;
        iblBindings[1].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        iblBindings[1].descriptorCount = 1;
        iblBindings[1].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;

        VkDescriptorSetLayoutCreateInfo dsl{};
        dsl.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        dsl.bindingCount = 2;
        dsl.pBindings    = iblBindings;
        if (vkCreateDescriptorSetLayout(m_gpu.device(), &dsl, nullptr, &m_iblDescLayout) != VK_SUCCESS)
            throw std::runtime_error("failed to create IBL descriptor set layout!");

        VkPushConstantRange pcr{};
        pcr.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        pcr.offset     = 0;
        pcr.size       = sizeof(IblPush);

        VkPipelineLayoutCreateInfo pli{};
        pli.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pli.setLayoutCount         = 1;
        pli.pSetLayouts            = &m_iblDescLayout;
        pli.pushConstantRangeCount = 1;
        pli.pPushConstantRanges    = &pcr;
        if (vkCreatePipelineLayout(m_gpu.device(), &pli, nullptr, &m_iblPipelineLayout) != VK_SUCCESS)
            throw std::runtime_error("failed to create IBL pipeline layout!");

        // Un set pa la irradiancia y uno por mip del prefiltrado: cada uno lleva
        // una storage image distinta, asi que no se pueden reutilizar.
        const uint32_t setCount = 1 + IBL_PREFILTER_MIPS;
        VkDescriptorPoolSize iblSizes[2]{};
        iblSizes[0].type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        iblSizes[0].descriptorCount = setCount;
        iblSizes[1].type            = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        iblSizes[1].descriptorCount = setCount;

        VkDescriptorPoolCreateInfo dpi{};
        dpi.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        dpi.poolSizeCount = 2;
        dpi.pPoolSizes    = iblSizes;
        dpi.maxSets       = setCount;
        if (vkCreateDescriptorPool(m_gpu.device(), &dpi, nullptr, &m_iblDescPool) != VK_SUCCESS)
            throw std::runtime_error("failed to create IBL descriptor pool!");

        auto makeIblPipeline = [&](const std::string& spv, VkPipeline& pipeline)
        {
            auto code   = loadShaderFile(spv);
            auto module = createShaderModule(code);

            VkComputePipelineCreateInfo ci{};
            ci.sType        = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
            ci.stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            ci.stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
            ci.stage.module = module;
            ci.stage.pName  = "main";
            ci.layout       = m_iblPipelineLayout;
            if (vkCreateComputePipelines(m_gpu.device(), VK_NULL_HANDLE, 1, &ci, nullptr, &pipeline) != VK_SUCCESS)
                throw std::runtime_error("failed to create compute pipeline: " + spv);

            vkDestroyShaderModule(m_gpu.device(), module, nullptr);
        };

        makeIblPipeline("shaders/ibl_irradiance.comp.spv", m_iblIrradiancePipeline);
        makeIblPipeline("shaders/ibl_prefilter.comp.spv",  m_iblPrefilterPipeline);
    }

    void Renderer::precomputeIbl()
    {
        // Sin cubemap de entorno no hay nada que convolucionar: se quedan los
        // valores neutros que dejo createIblResources.
        if (!m_skybox.isInitialized()) return;

        const auto t0 = std::chrono::steady_clock::now();

        // Reset y no free: initSkybox podria llamarse otra vez (cambio de
        // entorno) y los sets de la vez anterior ya no valen.
        vkResetDescriptorPool(m_gpu.device(), m_iblDescPool, 0);

        const uint32_t setCount = 1 + IBL_PREFILTER_MIPS;
        std::vector<VkDescriptorSetLayout> layouts(setCount, m_iblDescLayout);
        std::vector<VkDescriptorSet>       sets(setCount, VK_NULL_HANDLE);

        VkDescriptorSetAllocateInfo ai{};
        ai.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ai.descriptorPool     = m_iblDescPool;
        ai.descriptorSetCount = setCount;
        ai.pSetLayouts        = layouts.data();
        if (vkAllocateDescriptorSets(m_gpu.device(), &ai, sets.data()) != VK_SUCCESS)
            throw std::runtime_error("failed to allocate IBL descriptor sets!");

        VkDescriptorImageInfo envInfo{};
        envInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        envInfo.imageView   = m_skybox.cubeView();
        envInfo.sampler     = m_skybox.cubeSampler();

        // Los VkDescriptorImageInfo tienen que seguir vivos hasta el
        // vkUpdateDescriptorSets, asi que el vector se dimensiona de golpe.
        std::vector<VkDescriptorImageInfo>  storeInfos(setCount);
        std::vector<VkWriteDescriptorSet>   writes;
        writes.reserve(setCount * 2);
        for (uint32_t s = 0; s < setCount; s++)
        {
            storeInfos[s].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
            storeInfos[s].imageView   = (s == 0) ? m_iblIrradianceStore : m_iblPrefilterStore[s - 1];

            VkWriteDescriptorSet src{};
            src.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            src.dstSet          = sets[s];
            src.dstBinding      = 0;
            src.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            src.descriptorCount = 1;
            src.pImageInfo      = &envInfo;
            writes.push_back(src);

            VkWriteDescriptorSet dst = src;
            dst.dstBinding     = 1;
            dst.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            dst.pImageInfo     = &storeInfos[s];
            writes.push_back(dst);
        }
        vkUpdateDescriptorSets(m_gpu.device(), (uint32_t)writes.size(), writes.data(), 0, nullptr);

        VkCommandBuffer cmd = m_gpu.beginOneTimeCommands();

        VkImageMemoryBarrier barriers[2]{};
        for (int i = 0; i < 2; i++)
        {
            barriers[i].sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barriers[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barriers[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barriers[i].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            barriers[i].subresourceRange.layerCount = 6;
        }
        barriers[0].image = m_iblIrradianceImage;
        barriers[0].subresourceRange.levelCount = 1;
        barriers[1].image = m_iblPrefilterImage;
        barriers[1].subresourceRange.levelCount = IBL_PREFILTER_MIPS;

        // A GENERAL: es el unico layout que admite imageStore.
        for (int i = 0; i < 2; i++)
        {
            barriers[i].oldLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            barriers[i].newLayout     = VK_IMAGE_LAYOUT_GENERAL;
            barriers[i].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
            barriers[i].dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        }
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             0, 0, nullptr, 0, nullptr, 2, barriers);

        // Irradiancia: una invocacion por texel, 6 capas en z.
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_iblIrradiancePipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_iblPipelineLayout,
                                0, 1, &sets[0], 0, nullptr);
        // intensity 1.0: el IBL global no escala nada, asi que los dos cubemaps
        // salen bit a bit como antes de que el push llevara ese campo.
        IblPush push{ 0.0f, IBL_IRRADIANCE_SIZE, 1.0f };
        vkCmdPushConstants(cmd, m_iblPipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
        const uint32_t irrGroups = (IBL_IRRADIANCE_SIZE + 7) / 8;
        vkCmdDispatch(cmd, irrGroups, irrGroups, 6);

        // Prefiltrado: un dispatch por mip. Escriben regiones disjuntas y nadie
        // las lee entre medias, asi que no hacen falta barreras intermedias.
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_iblPrefilterPipeline);
        for (uint32_t m = 0; m < IBL_PREFILTER_MIPS; m++)
        {
            const uint32_t mipSize = IBL_PREFILTER_SIZE >> m;
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_iblPipelineLayout,
                                    0, 1, &sets[1 + m], 0, nullptr);
            IblPush mipPush{ (float)m / (float)(IBL_PREFILTER_MIPS - 1), mipSize, 1.0f };
            vkCmdPushConstants(cmd, m_iblPipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(mipPush), &mipPush);
            const uint32_t groups = (mipSize + 7) / 8;
            vkCmdDispatch(cmd, groups, groups, 6);
        }

        for (int i = 0; i < 2; i++)
        {
            barriers[i].oldLayout     = VK_IMAGE_LAYOUT_GENERAL;
            barriers[i].newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            barriers[i].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            barriers[i].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        }
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             0, 0, nullptr, 0, nullptr, 2, barriers);

        // Bloquea hasta que la cola termina, asi que el ms medido incluye la GPU.
        m_gpu.endOneTimeCommands(cmd);

        const double ms = std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - t0).count();
        printf("IBL precompute: %.2f ms (irradiance %ux%u, prefilter %ux%u x%u mips)\n",
               ms, IBL_IRRADIANCE_SIZE, IBL_IRRADIANCE_SIZE,
               IBL_PREFILTER_SIZE, IBL_PREFILTER_SIZE, IBL_PREFILTER_MIPS);
        fflush(stdout);
    }

    // ── Reflection probes ───────────────────────────────────────────────────
    // Nada de lo que hay aqui abajo graba un solo comando en el command buffer
    // del frame: el bake son submits propios, disparados por un evento. Con las
    // sondas ya bakeadas el frame cuesta exactamente lo mismo que con ninguna,
    // porque lo unico que cambia son DOS descriptores (bindings 5 y 6 del set 0)
    // que ya estaban ahi apuntando al IBL global.

    void Renderer::createProbeCapture()
    {
        // Cubemap intermedio del bake, UNO solo pa todas las sondas: solo tiene
        // que vivir entre el render de las 6 caras y la convolucion. Se crea la
        // primera vez que hay algo que bakear, asi que una escena sin sondas no
        // gasta ni un byte por esta feature.
        VkImageCreateInfo ci{};
        ci.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ci.flags         = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
        ci.imageType     = VK_IMAGE_TYPE_2D;
        ci.format        = kIblFormat;
        ci.extent        = { PROBE_FACE_SIZE, PROBE_FACE_SIZE, 1 };
        ci.mipLevels     = 1;
        ci.arrayLayers   = 6;
        ci.samples       = VK_SAMPLE_COUNT_1_BIT;
        ci.tiling        = VK_IMAGE_TILING_OPTIMAL;
        // TRANSFER_DST: destino del blit desde m_hdrImage, una cara por submit.
        ci.usage         = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        ci.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
        ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkCreateImage(m_gpu.device(), &ci, nullptr, &m_probeCaptureImage) != VK_SUCCESS)
            throw std::runtime_error("failed to create probe capture cubemap!");

        VkMemoryRequirements memReq;
        vkGetImageMemoryRequirements(m_gpu.device(), m_probeCaptureImage, &memReq);
        VkMemoryAllocateInfo memAlloc{};
        memAlloc.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        memAlloc.allocationSize  = memReq.size;
        memAlloc.memoryTypeIndex = m_gpu.findMemoryType(memReq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (vkAllocateMemory(m_gpu.device(), &memAlloc, nullptr, &m_probeCaptureMemory) != VK_SUCCESS)
            throw std::runtime_error("failed to allocate probe capture memory!");
        vkBindImageMemory(m_gpu.device(), m_probeCaptureImage, m_probeCaptureMemory, 0);

        VkImageViewCreateInfo vi{};
        vi.sType                           = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vi.image                           = m_probeCaptureImage;
        vi.viewType                        = VK_IMAGE_VIEW_TYPE_CUBE;
        vi.format                          = kIblFormat;
        vi.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        vi.subresourceRange.baseMipLevel   = 0;
        vi.subresourceRange.levelCount     = 1;
        vi.subresourceRange.baseArrayLayer = 0;
        vi.subresourceRange.layerCount     = 6;
        if (vkCreateImageView(m_gpu.device(), &vi, nullptr, &m_probeCaptureView) != VK_SUCCESS)
            throw std::runtime_error("failed to create probe capture view!");

        // Arranca en SHADER_READ_ONLY, que es el layout desde el que el bake la
        // mueve a TRANSFER_DST y al que la devuelve. El contenido inicial da
        // igual: el bake escribe las 6 caras antes de que nadie las lea.
        {
            VkCommandBuffer cmd = m_gpu.beginOneTimeCommands();
            VkImageMemoryBarrier b{};
            b.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image               = m_probeCaptureImage;
            b.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
            b.newLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            b.srcAccessMask       = 0;
            b.dstAccessMask       = VK_ACCESS_SHADER_READ_BIT;
            b.subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 6 };
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                 0, 0, nullptr, 0, nullptr, 1, &b);
            m_gpu.endOneTimeCommands(cmd);
        }

        // Query pool propio del bake: 7 pares (6 caras + convolucion). No se
        // mezcla con el del AA ni con el del bloom, que se resetean por frame.
        if (m_timestampsSupported && m_probeQueryPool == VK_NULL_HANDLE)
        {
            VkQueryPoolCreateInfo qi{};
            qi.sType      = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
            qi.queryType  = VK_QUERY_TYPE_TIMESTAMP;
            qi.queryCount = PROBE_QUERY_COUNT;
            if (vkCreateQueryPool(m_gpu.device(), &qi, nullptr, &m_probeQueryPool) != VK_SUCCESS)
                throw std::runtime_error("failed to create probe query pool!");
        }
    }

    void Renderer::createProbeImages(GpuProbe& probe)
    {
        // Mismas dos imagenes que el IBL global (createIblResources), pero por
        // sonda. m_res.createImage no vale: fija arrayLayers y mipLevels a 1.
        auto makeCube = [&](uint32_t size, uint32_t mips, VkImage& image, VkDeviceMemory& memory)
        {
            VkImageCreateInfo ci{};
            ci.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
            ci.flags         = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
            ci.imageType     = VK_IMAGE_TYPE_2D;
            ci.format        = kIblFormat;
            ci.extent        = { size, size, 1 };
            ci.mipLevels     = mips;
            ci.arrayLayers   = 6;
            ci.samples       = VK_SAMPLE_COUNT_1_BIT;
            ci.tiling        = VK_IMAGE_TILING_OPTIMAL;
            ci.usage         = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT
                             | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
            ci.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
            ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            if (vkCreateImage(m_gpu.device(), &ci, nullptr, &image) != VK_SUCCESS)
                throw std::runtime_error("failed to create probe cubemap image!");

            VkMemoryRequirements memReq;
            vkGetImageMemoryRequirements(m_gpu.device(), image, &memReq);
            VkMemoryAllocateInfo memAlloc{};
            memAlloc.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            memAlloc.allocationSize  = memReq.size;
            memAlloc.memoryTypeIndex = m_gpu.findMemoryType(memReq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            if (vkAllocateMemory(m_gpu.device(), &memAlloc, nullptr, &memory) != VK_SUCCESS)
                throw std::runtime_error("failed to allocate probe cubemap memory!");
            vkBindImageMemory(m_gpu.device(), image, memory, 0);
        };

        makeCube(IBL_IRRADIANCE_SIZE, 1,                  probe.irradianceImage, probe.irradianceMemory);
        makeCube(IBL_PREFILTER_SIZE,  IBL_PREFILTER_MIPS, probe.prefilterImage,  probe.prefilterMemory);

        auto makeView = [&](VkImage image, VkImageViewType type, uint32_t baseMip, uint32_t mipCount, VkImageView& view)
        {
            VkImageViewCreateInfo vi{};
            vi.sType                           = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            vi.image                           = image;
            vi.viewType                        = type;
            vi.format                          = kIblFormat;
            vi.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
            vi.subresourceRange.baseMipLevel   = baseMip;
            vi.subresourceRange.levelCount     = mipCount;
            vi.subresourceRange.baseArrayLayer = 0;
            vi.subresourceRange.layerCount     = 6;
            if (vkCreateImageView(m_gpu.device(), &vi, nullptr, &view) != VK_SUCCESS)
                throw std::runtime_error("failed to create probe image view!");
        };

        makeView(probe.irradianceImage, VK_IMAGE_VIEW_TYPE_CUBE,     0, 1, probe.irradianceView);
        makeView(probe.irradianceImage, VK_IMAGE_VIEW_TYPE_2D_ARRAY, 0, 1, probe.irradianceStore);
        makeView(probe.prefilterImage,  VK_IMAGE_VIEW_TYPE_CUBE,     0, IBL_PREFILTER_MIPS, probe.prefilterView);
        for (uint32_t m = 0; m < IBL_PREFILTER_MIPS; m++)
            makeView(probe.prefilterImage, VK_IMAGE_VIEW_TYPE_2D_ARRAY, m, 1, probe.prefilterStore[m]);

        // Contenido neutro, por el mismo motivo que en createIblResources: entre
        // que la sonda existe y que alguien pulsa Bake, sus vistas ya estan en
        // descriptor sets y no pueden apuntar a memoria sin definir. Los mismos
        // valores que el IBL neutro, asi que una sonda recien creada y sin
        // bakear se ve igual que el ambiente plano de siempre.
        {
            VkCommandBuffer cmd = m_gpu.beginOneTimeCommands();

            VkImageMemoryBarrier b{};
            b.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            b.subresourceRange.layerCount = 6;

            auto clearTo = [&](VkImage image, uint32_t mips, const VkClearColorValue& color)
            {
                b.image                       = image;
                b.subresourceRange.levelCount = mips;

                b.oldLayout     = VK_IMAGE_LAYOUT_UNDEFINED;
                b.newLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                b.srcAccessMask = 0;
                b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                     0, 0, nullptr, 0, nullptr, 1, &b);

                vkCmdClearColorImage(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                     &color, 1, &b.subresourceRange);

                b.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                b.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
                vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                     0, 0, nullptr, 0, nullptr, 1, &b);
            };

            const VkClearColorValue irradianceNeutral{{ 0.075f, 0.080f, 0.090f, 1.0f }};
            const VkClearColorValue prefilterNeutral {{ 0.100f, 0.120f, 0.150f, 1.0f }};
            clearTo(probe.irradianceImage, 1,                  irradianceNeutral);
            clearTo(probe.prefilterImage,  IBL_PREFILTER_MIPS, prefilterNeutral);

            m_gpu.endOneTimeCommands(cmd);
        }
    }

    void Renderer::destroyProbeImages(GpuProbe& probe)
    {
        // El caller ya ha esperado a que la GPU quede libre y ha reescrito los
        // bindings 5/6 que apuntaban aqui: al llegar a esta funcion ningun
        // descriptor set referencia estas vistas.
        vkDestroyImageView(m_gpu.device(), probe.irradianceView,  nullptr);
        vkDestroyImageView(m_gpu.device(), probe.irradianceStore, nullptr);
        vkDestroyImage(m_gpu.device(), probe.irradianceImage, nullptr);
        vkFreeMemory(m_gpu.device(), probe.irradianceMemory, nullptr);
        vkDestroyImageView(m_gpu.device(), probe.prefilterView, nullptr);
        for (uint32_t m = 0; m < IBL_PREFILTER_MIPS; m++)
            vkDestroyImageView(m_gpu.device(), probe.prefilterStore[m], nullptr);
        vkDestroyImage(m_gpu.device(), probe.prefilterImage, nullptr);
        vkFreeMemory(m_gpu.device(), probe.prefilterMemory, nullptr);
        probe = GpuProbe{};
    }

    void Renderer::writeIblBindings(VkDescriptorSet set, VkImageView irradiance, VkImageView prefilter)
    {
        // Un write suelto sobre un set YA alojado, igual que writeSsaoBinding:
        // reescribir los bindings 5 y 6 es lo unico que hace falta para que un
        // objeto pase del IBL global a una sonda. Ni layout nuevo, ni miembro
        // nuevo en el UBO, ni un indice en PushData (que esta a 80 bytes justos).
        VkDescriptorImageInfo infos[2]{};
        infos[0].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        infos[0].imageView   = irradiance;
        infos[0].sampler     = m_iblSampler;
        infos[1].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        infos[1].imageView   = prefilter;
        infos[1].sampler     = m_iblSampler;

        VkWriteDescriptorSet w[2]{};
        for (int i = 0; i < 2; i++)
        {
            w[i].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[i].dstSet          = set;
            w[i].dstBinding      = 5 + i;
            w[i].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            w[i].descriptorCount = 1;
            w[i].pImageInfo      = &infos[i];
        }
        vkUpdateDescriptorSets(m_gpu.device(), 2, w, 0, nullptr);
    }

    int Renderer::pickProbeFor(const glm::vec3& worldPos) const
    {
        // La sonda MAS CERCANA cuyo radio contiene el punto. -1 = ninguna, y
        // entonces el objeto se queda con el IBL global de siempre.
        int   best     = -1;
        float bestDist = 0.0f;
        for (size_t i = 0; i < m_probes.size(); i++)
        {
            const float d = glm::length(worldPos - m_probes[i].position);
            if (d > m_probes[i].radius) continue;
            if (best < 0 || d < bestDist) { best = (int)i; bestDist = d; }
        }
        return best;
    }

    void Renderer::refreshProbeAssignment()
    {
        // Calcula la asignacion DESEADA y solo toca la GPU si difiere de la ya
        // escrita. En regimen estacionario esto son unas cuantas restas de
        // vectores en CPU y cero trabajo de GPU: ni un comando, ni un write.
        std::unordered_map<int, int> wantShared;
        for (const auto& obj : m_objects)
        {
            const SharedGpuMesh* gpu = m_sharedMeshes.get(obj.sharedIndex);
            if (!gpu) continue;
            // El descriptor set es POR MALLA COMPARTIDA, no por GameObject: dos
            // instancias de la misma malla bajo sondas distintas comparten
            // sonda, y gana la del primer objeto del recorrido. Es el precio de
            // no duplicar los sets (y con el, el instancing).
            if (wantShared.find(obj.sharedIndex) != wantShared.end()) continue;
            const glm::vec3 local  = gpu->hasBounds ? (gpu->aabbMin + gpu->aabbMax) * 0.5f : glm::vec3(0.0f);
            const glm::vec3 center = glm::vec3(obj.transform * glm::vec4(local, 1.0f));
            wantShared[obj.sharedIndex] = pickProbeFor(center);
        }

        std::vector<int> wantSkinned(m_skinnedObjects.size(), -1);
        for (size_t i = 0; i < m_skinnedObjects.size(); i++)
            wantSkinned[i] = pickProbeFor(glm::vec3(m_skinnedObjects[i].transform[3]));

        if (wantShared == m_probeAssignShared && wantSkinned == m_probeAssignSkinned) return;

        // Hay cambios: los sets pueden estar en uso por frames en vuelo.
        vkDeviceWaitIdle(m_gpu.device());

        auto viewsFor = [&](int probeIndex, VkImageView& irr, VkImageView& pre)
        {
            if (probeIndex < 0 || probeIndex >= (int)m_probes.size())
            {
                irr = m_iblIrradianceView;
                pre = m_iblPrefilterView;
            }
            else
            {
                irr = m_probes[probeIndex].irradianceView;
                pre = m_probes[probeIndex].prefilterView;
            }
        };

        for (const auto& entry : wantShared)
        {
            auto prev = m_probeAssignShared.find(entry.first);
            if (prev != m_probeAssignShared.end() && prev->second == entry.second) continue;
            SharedGpuMesh* gpu = m_sharedMeshes.get(entry.first);
            if (!gpu) continue;
            VkImageView irr, pre;
            viewsFor(entry.second, irr, pre);
            for (int i = 0; i < MAX_FRAMES; i++)
                if (gpu->descriptorSets[i]) writeIblBindings(gpu->descriptorSets[i], irr, pre);
        }
        // Mallas que YA NO estan en el mapa deseado (objeto borrado) no hace
        // falta devolverlas al IBL global: sus sets se liberan con la malla.

        for (size_t si = 0; si < m_skinnedObjects.size(); si++)
        {
            const int want = wantSkinned[si];
            if (si < m_probeAssignSkinned.size() && m_probeAssignSkinned[si] == want) continue;
            VkImageView irr, pre;
            viewsFor(want, irr, pre);
            for (const SkinnedMatGfx& mgfx : m_skinnedObjects[si].matGfx)
                for (int i = 0; i < MAX_FRAMES; i++)
                    if (mgfx.descSets[i]) writeIblBindings(mgfx.descSets[i], irr, pre);
        }

        m_probeAssignShared  = std::move(wantShared);
        m_probeAssignSkinned = std::move(wantSkinned);
    }

    void Renderer::assignAllToGlobalIbl()
    {
        // Devuelve TODOS los objetos al IBL global. Se llama justo antes de una
        // tanda de bakes y no es un detalle: la captura reusa el pass de escena,
        // que ilumina cada objeto con lo que tenga en sus bindings 5/6. Si eso
        // es el cubemap de la propia sonda, cada bake vuelve a capturar la luz
        // que ya llevaba la intensidad aplicada y el efecto se amplifica bake a
        // bake (o se apaga, con intensidades bajas). Capturando siempre con el
        // IBL global el bake es idempotente y no depende del orden de las sondas.
        if (m_probeAssignShared.empty() && m_probeAssignSkinned.empty()) return;

        vkDeviceWaitIdle(m_gpu.device());
        for (int index : m_sharedMeshes.liveIndices())
        {
            SharedGpuMesh* gpu = m_sharedMeshes.get(index);
            for (int i = 0; i < MAX_FRAMES; i++)
                if (gpu->descriptorSets[i])
                    writeIblBindings(gpu->descriptorSets[i], m_iblIrradianceView, m_iblPrefilterView);
        }
        for (const SkinnedRenderObject& sobj : m_skinnedObjects)
            for (const SkinnedMatGfx& mgfx : sobj.matGfx)
                for (int i = 0; i < MAX_FRAMES; i++)
                    if (mgfx.descSets[i])
                        writeIblBindings(mgfx.descSets[i], m_iblIrradianceView, m_iblPrefilterView);

        // Las caches quedan vacias a proposito: refreshProbeAssignment, al final
        // de syncReflectionProbes, vuelve a escribir la asignacion real.
        m_probeAssignShared.clear();
        m_probeAssignSkinned.clear();
    }

    void Renderer::bakeProbe(GpuProbe& probe)
    {
        // Sin framebuffer de escena (init temprano) o sin el SSBO de instancias
        // no hay contra que dibujar: la peticion se reintenta en otro frame.
        if (m_offscreenFramebuffer[0] == VK_NULL_HANDLE) return;
        if (m_instanceDescSets[0]     == VK_NULL_HANDLE) return;

        if (m_probeCaptureImage == VK_NULL_HANDLE) createProbeCapture();

        // Las 6 caras dibujan sobre m_hdrImage[0] y leen el UBO del frame 0, que
        // pueden estar en vuelo. Esto es un evento, no un pass: se puede esperar.
        vkDeviceWaitIdle(m_gpu.device());

        const uint32_t faceRender = std::min(PROBE_FACE_SIZE,
                                             std::min(m_renderExtent.width, m_renderExtent.height));
        if (faceRender == 0) return;

        // Base: el UBO del frame 0 TAL CUAL. Luces, matrices de cascada y splits
        // se conservan a proposito — el shadow map que hay en la GPU es el de
        // esas matrices, y recomputarlas aqui lo descuadraria.
        UniformBufferObject ubo{};
        memcpy(&ubo, m_uniformBuffersMapped[0], sizeof(ubo));
        ubo.viewPos = glm::vec4(probe.position, 1.0f);

        // Forward+ a Off durante la captura: su rejilla se culleo contra el
        // frustum de la camara del frame, no contra estas 6 caras. mode 0 es el
        // bucle clasico sobre las luces del UBO, con todas ellas. Se restaura al
        // salir; el modo que la UI tiene pedido no se toca.
        FpParamsGpu savedFp{};
        bool restoreFp = false;
        if (m_fpParamsMapped[0])
        {
            memcpy(&savedFp, m_fpParamsMapped[0], sizeof(FpParamsGpu));
            FpParamsGpu off = savedFp;
            off.mode = 0;
            memcpy(m_fpParamsMapped[0], &off, sizeof(off));
            restoreFp = true;
        }

        // Direcciones y "up" de las 6 caras. Los up son los OPUESTOS a los de la
        // lista clasica de OpenGL, y la proyeccion invierte X ademas de la Y de
        // Vulkan: dos espejos son una rotacion, asi que el winding (y con el, el
        // face culling del pipeline) se conserva, y la cara sale con la
        // orientacion que espera el muestreo de un samplerCube.
        static const glm::vec3 kDirs[6] = {
            {  1.0f,  0.0f,  0.0f }, { -1.0f,  0.0f,  0.0f },
            {  0.0f,  1.0f,  0.0f }, {  0.0f, -1.0f,  0.0f },
            {  0.0f,  0.0f,  1.0f }, {  0.0f,  0.0f, -1.0f },
        };
        static const glm::vec3 kUps[6] = {
            {  0.0f,  1.0f,  0.0f }, {  0.0f,  1.0f,  0.0f },
            {  0.0f,  0.0f, -1.0f }, {  0.0f,  0.0f,  1.0f },
            {  0.0f,  1.0f,  0.0f }, {  0.0f,  1.0f,  0.0f },
        };

        VkImageMemoryBarrier b{};
        b.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;

        for (uint32_t face = 0; face < 6; face++)
        {
            glm::mat4 proj = glm::perspectiveRH_ZO(glm::radians(90.0f), 1.0f,
                                                   1.0f, 20000.0f);
            proj[0][0] *= -1.0f;
            proj[1][1] *= -1.0f;
            ubo.view = glm::lookAtRH(probe.position, probe.position + kDirs[face], kUps[face]);
            ubo.proj = proj;
            memcpy(m_uniformBuffersMapped[0], &ubo, sizeof(ubo));

            VkCommandBuffer cmd = m_gpu.beginOneTimeCommands();

            if (m_timestampsSupported && m_probeQueryPool != VK_NULL_HANDLE)
            {
                // Reset unico de las 14 en el primer submit: los writes de los
                // submits siguientes van detras en la misma cola.
                if (face == 0) vkCmdResetQueryPool(cmd, m_probeQueryPool, 0, PROBE_QUERY_COUNT);
                vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, m_probeQueryPool, face * 2);
            }

            // El mapa de AO que hay en la GPU es el de la camara del frame: dejarlo
            // hornearia oclusion de otro punto de vista dentro del cubemap. A 1.0
            // = sin oclusion; el frame siguiente lo recalcula si el SSAO esta
            // activo, y si no lo esta ya valia 1.0.
            if (face == 0 && m_ssaoBlurImage[0] != VK_NULL_HANDLE)
            {
                // oldLayout UNDEFINED y no GENERAL: si el SSAO nunca ha corrido
                // sobre este slot la imagen no se ha transicionado nunca, y los
                // draws de aqui abajo la muestrean por el binding 7 (declarado
                // GENERAL). Descartar el contenido no cuesta nada: se limpia.
                VkImageMemoryBarrier ao{};
                ao.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                ao.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                ao.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                ao.image               = m_ssaoBlurImage[0];
                ao.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
                ao.newLayout           = VK_IMAGE_LAYOUT_GENERAL;
                ao.srcAccessMask       = 0;
                ao.dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
                ao.subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
                vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                     0, 0, nullptr, 0, nullptr, 1, &ao);

                const VkClearColorValue white{{ 1.0f, 1.0f, 1.0f, 1.0f }};
                vkCmdClearColorImage(cmd, m_ssaoBlurImage[0], VK_IMAGE_LAYOUT_GENERAL,
                                     &white, 1, &ao.subresourceRange);

                ao.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                ao.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
                ao.oldLayout     = VK_IMAGE_LAYOUT_GENERAL;
                vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                     0, 0, nullptr, 0, nullptr, 1, &ao);
            }

            VkClearValue clearValues[2];
            clearValues[0].color        = {0.0f, 0.0f, 0.0f, 1.0f};
            clearValues[1].depthStencil = {1.0f, 0};

            VkRenderPassBeginInfo rpInfo{};
            rpInfo.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
            rpInfo.renderPass        = m_offscreenRenderPass;
            rpInfo.framebuffer       = m_offscreenFramebuffer[0];
            // Cuadrada y en la esquina: el framebuffer es el del viewport (16:9
            // con cualquier suerte) y una cara de cubemap tiene que salir de una
            // proyeccion de aspecto 1. El resto del framebuffer ni se toca.
            rpInfo.renderArea.offset = {0, 0};
            rpInfo.renderArea.extent = { faceRender, faceRender };
            rpInfo.clearValueCount   = 2;
            rpInfo.pClearValues      = clearValues;
            vkCmdBeginRenderPass(cmd, &rpInfo, VK_SUBPASS_CONTENTS_INLINE);

            VkViewport viewport{};
            viewport.width    = (float)faceRender;
            viewport.height   = (float)faceRender;
            viewport.minDepth = 0.0f;
            viewport.maxDepth = 1.0f;
            vkCmdSetViewport(cmd, 0, 1, &viewport);

            VkRect2D scissor{};
            scissor.extent = { faceRender, faceRender };
            vkCmdSetScissor(cmd, 0, 1, &scissor);

            // Pipeline de escena de siempre. El wireframe NO se respeta aqui a
            // proposito: lo que se captura es el entorno iluminado, no la ayuda
            // de edicion.
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);
            // Set 1: el SSBO de instancias sigue siendo obligatorio (el vertex
            // shader lo declara), aunque aqui no se instancie nada.
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                m_pipelineLayout, 1, 1, &m_instanceDescSets[0], 0, nullptr);
            if (m_fpSets[0] != VK_NULL_HANDLE)
            {
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    m_pipelineLayout, 2, 1, &m_fpSets[0], 0, nullptr);
            }

            for (const auto& obj : m_objects)
            {
                const SharedGpuMesh* gpu = m_sharedMeshes.get(obj.sharedIndex);
                if (!gpu || gpu->uploadTicket > m_lastCompletedTicket) continue;
                if (!gpu->descriptorSets[0]) continue;
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    m_pipelineLayout, 0, 1, &gpu->descriptorSets[0], 0, nullptr);
                PushData push;
                // Sin instancing (flags.x = 0): la matriz va en el push, que es
                // la ruta que ya usan los skinned. Asi el bake no toca el SSBO
                // del frame ni su cursor.
                push.transform = obj.transform;
                push.metallic  = gpu->metallic;
                push.roughness = gpu->roughness;
                push.flags.x   = 0.0f;
                // flags.y = 0: el alfa del HDR es la mascara de SSR y aqui no hay
                // pass de SSR que la lea.
                push.flags.y   = 0.0f;
                vkCmdPushConstants(cmd, m_pipelineLayout,
                    VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                    0, sizeof(PushData), &push);
                VkBuffer vbs[]      = { gpu->vertexBuffer };
                VkDeviceSize offs[] = { 0 };
                vkCmdBindVertexBuffers(cmd, 0, 1, vbs, offs);
                vkCmdBindIndexBuffer(cmd, gpu->indexBuffer, 0, VK_INDEX_TYPE_UINT32);
                vkCmdDrawIndexed(cmd, gpu->indexCount, 1, 0, 0, 0);
            }

            if (!m_skinnedObjects.empty())
            {
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_skinnedGfxPipeline);
                for (SkinnedRenderObject& sobj : m_skinnedObjects)
                {
                    if (sobj.outputVertexBuffer == VK_NULL_HANDLE) continue;
                    if (sobj.uploadTicket > m_lastCompletedTicket) continue;
                    VkBuffer     vbs[]  = { sobj.outputVertexBuffer };
                    VkDeviceSize offs[] = { 0 };
                    vkCmdBindVertexBuffers(cmd, 0, 1, vbs, offs);
                    vkCmdBindIndexBuffer(cmd, sobj.indexBuffer, 0, VK_INDEX_TYPE_UINT32);
                    for (auto& sm : sobj.subMeshes)
                    {
                        SkinnedMatGfx& mgfx = sobj.matGfx[sm.materialIndex];
                        if (!mgfx.descSets[0]) continue;
                        PushData push;
                        push.transform = sobj.transform;
                        push.metallic  = mgfx.metallic;
                        push.roughness = mgfx.roughness;
                        push.flags.y   = 0.0f;
                        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            m_pipelineLayout, 0, 1, &mgfx.descSets[0], 0, nullptr);
                        vkCmdPushConstants(cmd, m_pipelineLayout,
                            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                            0, sizeof(PushData), &push);
                        vkCmdDrawIndexed(cmd, sm.indexCount, 1, sm.indexStart, 0, 0);
                    }
                }
            }

            if (m_skybox.isInitialized())
            {
                glm::mat4 rotView     = glm::mat4(glm::mat3(ubo.view));
                glm::mat4 invViewProj = glm::inverse(ubo.proj * rotView);
                m_skybox.draw(cmd, invViewProj);
            }

            vkCmdEndRenderPass(cmd);

            // ── Cara -> capa del cubemap de captura ──────────────────────────
            // El pass deja m_hdrImage en SHADER_READ_ONLY (finalLayout del
            // attachment de resolve, y del de color sin MSAA).
            b.image            = m_hdrImage[0];
            b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            b.oldLayout        = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            b.newLayout        = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            b.srcAccessMask    = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            b.dstAccessMask    = VK_ACCESS_TRANSFER_READ_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);

            VkImageMemoryBarrier toDst = b;
            toDst.image            = m_probeCaptureImage;
            toDst.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, face, 1 };
            toDst.oldLayout        = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            toDst.newLayout        = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            toDst.srcAccessMask    = VK_ACCESS_SHADER_READ_BIT;
            toDst.dstAccessMask    = VK_ACCESS_TRANSFER_WRITE_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toDst);

            // Blit y no copy: el render sale a faceRender (recortado por el
            // tamano del viewport) y la cara del cubemap es siempre de
            // PROBE_FACE_SIZE, asi que hay que escalar.
            VkImageBlit blit{};
            blit.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            blit.srcOffsets[0]  = { 0, 0, 0 };
            blit.srcOffsets[1]  = { (int32_t)faceRender, (int32_t)faceRender, 1 };
            blit.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, face, 1 };
            blit.dstOffsets[0]  = { 0, 0, 0 };
            blit.dstOffsets[1]  = { (int32_t)PROBE_FACE_SIZE, (int32_t)PROBE_FACE_SIZE, 1 };
            vkCmdBlitImage(cmd,
                           m_hdrImage[0],         VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           m_probeCaptureImage,   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           1, &blit, VK_FILTER_LINEAR);

            // De vuelta a los layouts de partida: m_hdrImage la lee el bloom y
            // la composicion del frame siguiente, y la captura la lee el compute.
            toDst.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            toDst.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            toDst.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            toDst.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toDst);

            b.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            b.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            b.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);

            if (m_timestampsSupported && m_probeQueryPool != VK_NULL_HANDLE)
                vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, m_probeQueryPool, face * 2 + 1);

            // Bloquea hasta que la cola vacia: el UBO del frame 0 se reescribe
            // en la vuelta siguiente y no puede pisarse un draw en vuelo.
            m_gpu.endOneTimeCommands(cmd);
        }

        // ── Convolucion: los MISMOS dos compute del IBL global ──────────────
        {
            vkResetDescriptorPool(m_gpu.device(), m_iblDescPool, 0);

            const uint32_t setCount = 1 + IBL_PREFILTER_MIPS;
            std::vector<VkDescriptorSetLayout> layouts(setCount, m_iblDescLayout);
            std::vector<VkDescriptorSet>       sets(setCount, VK_NULL_HANDLE);

            VkDescriptorSetAllocateInfo ai{};
            ai.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            ai.descriptorPool     = m_iblDescPool;
            ai.descriptorSetCount = setCount;
            ai.pSetLayouts        = layouts.data();
            if (vkAllocateDescriptorSets(m_gpu.device(), &ai, sets.data()) != VK_SUCCESS)
                throw std::runtime_error("failed to allocate probe descriptor sets!");

            VkDescriptorImageInfo envInfo{};
            envInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            envInfo.imageView   = m_probeCaptureView;
            envInfo.sampler     = m_iblSampler;

            std::vector<VkDescriptorImageInfo> storeInfos(setCount);
            std::vector<VkWriteDescriptorSet>  writes;
            writes.reserve(setCount * 2);
            for (uint32_t s = 0; s < setCount; s++)
            {
                storeInfos[s].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
                storeInfos[s].imageView   = (s == 0) ? probe.irradianceStore : probe.prefilterStore[s - 1];

                VkWriteDescriptorSet src{};
                src.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                src.dstSet          = sets[s];
                src.dstBinding      = 0;
                src.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                src.descriptorCount = 1;
                src.pImageInfo      = &envInfo;
                writes.push_back(src);

                VkWriteDescriptorSet dst = src;
                dst.dstBinding     = 1;
                dst.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
                dst.pImageInfo     = &storeInfos[s];
                writes.push_back(dst);
            }
            vkUpdateDescriptorSets(m_gpu.device(), (uint32_t)writes.size(), writes.data(), 0, nullptr);

            VkCommandBuffer cmd = m_gpu.beginOneTimeCommands();
            if (m_timestampsSupported && m_probeQueryPool != VK_NULL_HANDLE)
                vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, m_probeQueryPool, 12);

            VkImageMemoryBarrier conv[2]{};
            for (int i = 0; i < 2; i++)
            {
                conv[i].sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                conv[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                conv[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                conv[i].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                conv[i].subresourceRange.layerCount = 6;
            }
            conv[0].image = probe.irradianceImage;
            conv[0].subresourceRange.levelCount = 1;
            conv[1].image = probe.prefilterImage;
            conv[1].subresourceRange.levelCount = IBL_PREFILTER_MIPS;

            for (int i = 0; i < 2; i++)
            {
                conv[i].oldLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                conv[i].newLayout     = VK_IMAGE_LAYOUT_GENERAL;
                conv[i].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
                conv[i].dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            }
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                 0, 0, nullptr, 0, nullptr, 2, conv);

            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_iblIrradiancePipeline);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_iblPipelineLayout,
                                    0, 1, &sets[0], 0, nullptr);
            IblPush push{ 0.0f, IBL_IRRADIANCE_SIZE, probe.intensity };
            vkCmdPushConstants(cmd, m_iblPipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
            const uint32_t irrGroups = (IBL_IRRADIANCE_SIZE + 7) / 8;
            vkCmdDispatch(cmd, irrGroups, irrGroups, 6);

            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_iblPrefilterPipeline);
            for (uint32_t m = 0; m < IBL_PREFILTER_MIPS; m++)
            {
                const uint32_t mipSize = IBL_PREFILTER_SIZE >> m;
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_iblPipelineLayout,
                                        0, 1, &sets[1 + m], 0, nullptr);
                IblPush mipPush{ (float)m / (float)(IBL_PREFILTER_MIPS - 1), mipSize, probe.intensity };
                vkCmdPushConstants(cmd, m_iblPipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(mipPush), &mipPush);
                const uint32_t groups = (mipSize + 7) / 8;
                vkCmdDispatch(cmd, groups, groups, 6);
            }

            for (int i = 0; i < 2; i++)
            {
                conv[i].oldLayout     = VK_IMAGE_LAYOUT_GENERAL;
                conv[i].newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                conv[i].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
                conv[i].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            }
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                 0, 0, nullptr, 0, nullptr, 2, conv);

            if (m_timestampsSupported && m_probeQueryPool != VK_NULL_HANDLE)
                vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, m_probeQueryPool, 13);

            m_gpu.endOneTimeCommands(cmd);
        }

        if (restoreFp) memcpy(m_fpParamsMapped[0], &savedFp, sizeof(savedFp));

        // El UBO del frame 0 se queda con la ultima cara; updateUniformBuffer lo
        // reescribe entero antes del proximo submit del frame, asi que no hace
        // falta restaurarlo.

        probe.baked  = true;
        probe.bakeMs = 0.0f;
        if (m_timestampsSupported && m_probeQueryPool != VK_NULL_HANDLE)
        {
            uint64_t stamps[PROBE_QUERY_COUNT] = {};
            // WAIT_BIT y no polling: la cola ya esta vacia (endOneTimeCommands
            // bloquea), asi que los 14 resultados estan listos.
            if (vkGetQueryPoolResults(m_gpu.device(), m_probeQueryPool, 0, PROBE_QUERY_COUNT,
                                      sizeof(stamps), stamps, sizeof(uint64_t),
                                      VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) == VK_SUCCESS)
            {
                // Suma de los 7 deltas y no ultimo-menos-primero: entre submits
                // hay esperas del host que no son coste de GPU.
                double total = 0.0;
                for (uint32_t p = 0; p < PROBE_QUERY_COUNT / 2; p++)
                    total += (double)(stamps[p * 2 + 1] - stamps[p * 2]) * m_timestampPeriod * 1e-6;
                probe.bakeMs = (float)total;
            }
        }
    }

    void Renderer::syncReflectionProbes()
    {
        // Camino rapido: ni sondas en la escena ni nada que deshacer. Es el caso
        // de TODAS las escenas de hoy, y sale de aqui sin tocar la GPU.
        const bool nothingToDo = m_probes.empty() && m_probeAssignShared.empty()
                              && m_probeAssignSkinned.empty() && m_probeBakeQueue.empty()
                              && !m_probeBakeAllQueued;
        if (!m_scene && nothingToDo) return;

        // 1. Reconciliar la lista de sondas con la escena. Es lo unico que corre
        //    por frame cuando hay sondas: un recorrido del arbol (el mismo que
        //    ya hacen el gizmo y la fisica) y unas comparaciones de float.
        struct Desc { uint64_t id; glm::vec3 pos; float radius; float intensity; };
        std::vector<Desc> descs;
        if (m_scene)
        {
            m_scene->traverse([&](GameObject* go) {
                if (!go->hasReflectionProbe()) return;
                const auto& p = go->getReflectionProbe();
                descs.push_back({ go->id, glm::vec3(go->worldTransform[3]),
                                  p->getRadius(), p->getIntensity() });
            });
        }
        if (descs.empty() && nothingToDo) return;

        // Bajas: sondas cuyo GameObject ya no esta (borrado o cambio de escena).
        for (size_t i = m_probes.size(); i-- > 0; )
        {
            bool alive = false;
            for (const Desc& d : descs) if (d.id == m_probes[i].ownerId) { alive = true; break; }
            if (alive) continue;
            // ANTES de destruir: devolver al IBL global todo lo que apuntaba a
            // esta sonda, o quedarian descriptor sets con vistas muertas. Se
            // borra de la lista primero para que pickProbeFor ya no la elija.
            GpuProbe dying = m_probes[i];
            m_probes.erase(m_probes.begin() + (long)i);
            m_probeAssignShared.clear();     // fuerza la reescritura de todos
            m_probeAssignSkinned.clear();
            refreshProbeAssignment();
            vkDeviceWaitIdle(m_gpu.device());
            destroyProbeImages(dying);
        }

        // Altas y cambios de ajustes.
        bool geometryChanged = false;
        for (const Desc& d : descs)
        {
            GpuProbe* found = nullptr;
            for (GpuProbe& p : m_probes) if (p.ownerId == d.id) { found = &p; break; }
            if (!found)
            {
                GpuProbe fresh{};
                fresh.ownerId = d.id;
                createProbeImages(fresh);
                m_probes.push_back(fresh);
                found = &m_probes.back();
                geometryChanged = true;
            }
            if (found->position != d.pos || found->radius != d.radius)
                geometryChanged = true;
            // Mover la sonda invalida lo capturado, y cambiar la intensidad
            // invalida el cubemap convolucionado (la intensidad se hornea en
            // el). El radio NO: solo cambia a quien afecta, no lo que se ve.
            const bool dirty = (found->position != d.pos) || (found->intensity != d.intensity);
            if (dirty) { found->baked = false; found->settleFrames = 0; }
            else if (!found->baked) found->settleFrames++;
            found->position  = d.pos;
            found->radius    = d.radius;
            found->intensity = d.intensity;
        }
        (void)geometryChanged;

        // 2. Bakes. Sin un frame previo el UBO del slot 0 es basura (no lleva ni
        //    luces ni las matrices del shadow map): las peticiones esperan.
        if (m_uboWritten[0])
        {
            const bool bakeAll = m_probeBakeAllQueued;
            m_probeBakeAllQueued = false;
            std::vector<uint64_t> queue;
            queue.swap(m_probeBakeQueue);

            std::vector<GpuProbe*> toBake;
            for (GpuProbe& p : m_probes)
            {
                if (bakeAll || std::find(queue.begin(), queue.end(), p.ownerId) != queue.end())
                {
                    toBake.push_back(&p);
                    continue;
                }
                // Auto-bake de las que no tienen captura valida: es lo que hace
                // que cargar una escena (o arrancar DonTopoRuntime) de la misma
                // imagen que el editor sin pulsar nada. settleFrames espera a
                // que los ajustes dejen de moverse, asi que arrastrar un slider
                // no dispara un bake por frame: solo uno al soltar.
                if (!p.baked && p.settleFrames >= 1) toBake.push_back(&p);
            }

            float total = 0.0f;
            int   count = 0;
            if (!toBake.empty())
            {
                // ANTES de capturar nada: si no, la escena se fotografia
                // iluminada por las propias sondas y el efecto se realimenta.
                assignAllToGlobalIbl();
                for (GpuProbe* p : toBake)
                {
                    bakeProbe(*p);
                    if (p->baked) { total += p->bakeMs; count++; }
                }
            }
            if (count > 0)
            {
                m_probeLastBakeMs = total;
                printf("reflection probes: bake de %d sonda(s) en %.2f ms de GPU "
                       "(captura %ux%u x6, irradiancia %ux%u, prefiltrado %ux%u x%u mips, "
                       "%.2f MB por sonda)\n",
                       count, total, PROBE_FACE_SIZE, PROBE_FACE_SIZE,
                       IBL_IRRADIANCE_SIZE, IBL_IRRADIANCE_SIZE,
                       IBL_PREFILTER_SIZE, IBL_PREFILTER_SIZE, IBL_PREFILTER_MIPS,
                       (double)probeMemoryBytes() / (1024.0 * 1024.0));
                fflush(stdout);
            }
        }

        // 3. Asignacion sonda->objeto. Sale sin escribir nada si no ha cambiado.
        refreshProbeAssignment();
    }

    void Renderer::createComputePipelines()
    {
        // --- Descriptor set layout: 8 storage buffers ---
        VkDescriptorSetLayoutBinding bindings[8]{};
        for (uint32_t i = 0; i < 8; i++)
        {
            bindings[i].binding         = i;
            bindings[i].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            bindings[i].descriptorCount = 1;
            bindings[i].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
        }

        VkDescriptorSetLayoutCreateInfo dslInfo{};
        dslInfo.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        dslInfo.bindingCount = 8;
        dslInfo.pBindings    = bindings;
        // Guarda de idempotencia: recreateMsaaDependentPipelines vuelve a entrar
        // aqui para rehacer los cuatro pipelines skinned del final con otro
        // numero de muestras. Ni el layout, ni el pool, ni los descriptor sets ya
        // alojados de las mallas dependen de eso: recrearlos seria una fuga y
        // dejaria colgados los sets de todas las mallas skinned vivas.
        if (m_computeDescLayout == VK_NULL_HANDLE &&
            vkCreateDescriptorSetLayout(m_gpu.device(), &dslInfo, nullptr, &m_computeDescLayout) != VK_SUCCESS)
            throw std::runtime_error("failed to create compute descriptor set layout!");

        // --- Pipeline layout (1 set + push constant) ---
        VkPushConstantRange pcr{};
        pcr.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        pcr.offset     = 0;
        pcr.size       = sizeof(ComputePush);

        VkPipelineLayoutCreateInfo plInfo{};
        plInfo.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        plInfo.setLayoutCount         = 1;
        plInfo.pSetLayouts            = &m_computeDescLayout;
        plInfo.pushConstantRangeCount = 1;
        plInfo.pPushConstantRanges    = &pcr;
        if (m_computePipelineLayout == VK_NULL_HANDLE &&
            vkCreatePipelineLayout(m_gpu.device(), &plInfo, nullptr, &m_computePipelineLayout) != VK_SUCCESS)
        {
            throw std::runtime_error("failed to create compute pipeline layout!");
        }

        // --- Descriptor pool: 8 SSBOs * 16 objetos max ---
        VkDescriptorPoolSize ps{};
        ps.type            = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        ps.descriptorCount = 8 * 16;

        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.poolSizeCount = 1;
        poolInfo.pPoolSizes    = &ps;
        poolInfo.maxSets       = 16;
        poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        if (m_computeDescPool == VK_NULL_HANDLE &&
            vkCreateDescriptorPool(m_gpu.device(), &poolInfo, nullptr, &m_computeDescPool) != VK_SUCCESS)
        {
            throw std::runtime_error("failed to create compute descriptor pool!");
        }

        // --- Crear los tres pipelines ---
        auto makePipeline = [&](const std::string& spv, VkPipeline& pipeline)
        {
            auto code   = loadShaderFile(spv);
            auto module = createShaderModule(code);

            VkComputePipelineCreateInfo info{};
            info.sType        = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
            info.stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            info.stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
            info.stage.module = module;
            info.stage.pName  = "main";
            info.layout       = m_computePipelineLayout;

            if (vkCreateComputePipelines(m_gpu.device(), VK_NULL_HANDLE, 1, &info, nullptr, &pipeline) != VK_SUCCESS)
                throw std::runtime_error("failed to create compute pipeline: " + spv);

            vkDestroyShaderModule(m_gpu.device(), module, nullptr);
        };

        makePipeline("shaders/bone_eval.comp.spv",      m_boneEvalPipeline);
        makePipeline("shaders/bone_hierarchy.comp.spv", m_boneHierarchyPipeline);
        makePipeline("shaders/skinning.comp.spv",       m_skinningPipeline);

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
        if (obj.computeDescSet != VK_NULL_HANDLE)
        {
            vkFreeDescriptorSets(m_gpu.device(), m_computeDescPool, 1, &obj.computeDescSet);
            obj.computeDescSet = VK_NULL_HANDLE;
        }
        for (auto& mgfx : obj.matGfx)
        {
            if (mgfx.descSets[0] != VK_NULL_HANDLE)
                vkFreeDescriptorSets(m_gpu.device(), m_descriptorPool, MAX_FRAMES, mgfx.descSets);
            mgfx.descSets[0] = VK_NULL_HANDLE;
            mgfx.descSets[1] = VK_NULL_HANDLE;
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
        VkDescriptorSetAllocateInfo dsAlloc{};
        dsAlloc.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        dsAlloc.descriptorPool     = m_computeDescPool;
        dsAlloc.descriptorSetCount = 1;
        dsAlloc.pSetLayouts        = &m_computeDescLayout;
        if (vkAllocateDescriptorSets(m_gpu.device(), &dsAlloc, &obj.computeDescSet) != VK_SUCCESS)
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
            VkDescriptorSetLayout layouts[MAX_FRAMES] = { m_descriptorSetLayout, m_descriptorSetLayout };
            VkDescriptorSetAllocateInfo gfxAlloc{};
            gfxAlloc.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            gfxAlloc.descriptorPool     = m_descriptorPool;
            gfxAlloc.descriptorSetCount = MAX_FRAMES;
            gfxAlloc.pSetLayouts        = layouts;
            if (vkAllocateDescriptorSets(m_gpu.device(), &gfxAlloc, mgfx.descSets) != VK_SUCCESS)
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
                shdInfo.imageView   = m_shadowView;
                shdInfo.sampler     = m_shadowSampler;

                VkDescriptorImageInfo ormInfo{};
                ormInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                ormInfo.imageView   = mgfx.ormView;
                ormInfo.sampler     = mgfx.ormSampler;

                VkDescriptorImageInfo irrInfo{};
                irrInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                irrInfo.imageView   = m_iblIrradianceView;
                irrInfo.sampler     = m_iblSampler;

                VkDescriptorImageInfo preInfo{};
                preInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                preInfo.imageView   = m_iblPrefilterView;
                preInfo.sampler     = m_iblSampler;

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

    void Renderer::recordComputePass(VkCommandBuffer cmd)
    {
        if (m_skinnedObjects.empty()) return;

        auto ssboBarrier = [](VkBuffer buf) {
            VkBufferMemoryBarrier b{};
            b.sType         = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
            b.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            b.buffer        = buf;
            b.offset        = 0;
            b.size          = VK_WHOLE_SIZE;
            return b;
        };

        for (size_t i = 0; i < m_skinnedObjects.size(); i++)
        {
            // Borrado desde el editor, aún en vuelo (despachar skinning sobre un
            // SSBO cuyo batch no ha señalado sería un read-after-write que la
            // validación de sync marca) o fuera de cámara: los tres casos los
            // resolvió el culling del principio del frame, y el bucle de dibujo
            // de más abajo lee ESA misma lista. Saltar aquí un objeto que sí se
            // dibujara le dejaría la pose del último frame en que fue visible.
            if (i >= m_skinnedVisible.size() || !m_skinnedVisible[i]) continue;
            SkinnedRenderObject& obj = m_skinnedObjects[i];
            // Checkbox "Visible" apagado: no se dibuja en ningún pass, así que
            // skinearlo sería trabajo de GPU que nadie lee. La pose se queda
            // congelada en la del último frame visible, igual que hace el culling
            // con un personaje fuera de cámara.
            if (!obj.meshVisible) continue;
            ComputePush push{};
            push.animTime    = obj.animTime;
            push.boneCount   = obj.boneCount;
            push.vertexCount = obj.vertexCount;
            push.clipBase    = obj.activeClip * obj.boneCount;

            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                m_computePipelineLayout, 0, 1, &obj.computeDescSet, 0, nullptr);

            // --- Pass 1: bone_eval (local transforms) ---
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_boneEvalPipeline);
            vkCmdPushConstants(cmd, m_computePipelineLayout,
                VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(ComputePush), &push);
            vkCmdDispatch(cmd, (obj.boneCount + 63) / 64, 1, 1);

            // Barrier: localTransform escrito → leído por bone_hierarchy
            VkBufferMemoryBarrier b1 = ssboBarrier(obj.localTransformBuffer);
            vkCmdPipelineBarrier(cmd,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                0, 0, nullptr, 1, &b1, 0, nullptr);

            // --- Pass 2: bone_hierarchy (world transforms + inverse bind pose) ---
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_boneHierarchyPipeline);
            vkCmdPushConstants(cmd, m_computePipelineLayout,
                VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(ComputePush), &push);
            vkCmdDispatch(cmd, 1, 1, 1);

            // Barrier: finalBone escrito → leído por skinning
            VkBufferMemoryBarrier b2 = ssboBarrier(obj.finalBoneBuffer);
            vkCmdPipelineBarrier(cmd,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                0, 0, nullptr, 1, &b2, 0, nullptr);

            // --- Pass 3: skinning (output vertex buffer) ---
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_skinningPipeline);
            vkCmdPushConstants(cmd, m_computePipelineLayout,
                VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(ComputePush), &push);
            vkCmdDispatch(cmd, (obj.vertexCount + 63) / 64, 1, 1);

            // Barrier: outputVertexBuffer escrito por compute → leído como VB en vertex shader
            VkBufferMemoryBarrier b3{};
            b3.sType         = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
            b3.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            b3.dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT;
            b3.buffer        = obj.outputVertexBuffer;
            b3.offset        = 0;
            b3.size          = VK_WHOLE_SIZE;
            vkCmdPipelineBarrier(cmd,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_PIPELINE_STAGE_VERTEX_INPUT_BIT,
                0, 0, nullptr, 1, &b3, 0, nullptr);
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
        // m_ssaoDepthView, la profundidad del pre-pass. Sus tres imagenes van al
        // tamano interno del render, igual que el resto de targets intermedios.
        createSsaoImages();
        // DETRAS de createSsaoImages: el descriptor set del culling referencia
        // m_ssaoDepthView, que se acaba de crear ahi. La rejilla se dimensiona con
        // m_renderExtent, igual que el resto de targets intermedios.
        createFpBuffers();
        // ANTES de los framebuffers de escena y composicion: con MSAA sus
        // attachments de color son las imagenes multisample que se crean ahi.
        createAaImages();

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
        // m_ssaoDepthView (el de la marcha), asi que va detras de los dos.
        createSsrImages();
        // Depende de m_hdrView y de m_ssaoDepthView igual que el SSR. En el
        // primer init sale por la guarda (el UBO aun no existe) y lo rehace el
        // final de init.
        m_fogPass.createSets(fogCtx());
        // Depende de m_hdrView y de m_ssaoDepthView igual que el SSR.
        m_motionBlurPass.createImages(motionBlurCtx());
        printf("offscreen images OK\n"); fflush(stdout);
    }

    void Renderer::destroyOffscreenImages()
    {
        destroyBloomImages();
        destroySsaoImages();
        destroyFpBuffers();
        destroySsrImages();
        m_fogPass.destroySets();
        m_motionBlurPass.destroyImages(motionBlurCtx());
        destroyAaImages();
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

    void Renderer::createBloomPipelines()
    {
        // Sampler comun de toda la cadena. CLAMP_TO_EDGE es obligatorio: con
        // repeat, los taps del borde del filtro traerian el brillo del lado
        // opuesto de la pantalla y se veria sangrar luz por los bordes.
        VkSamplerCreateInfo si{};
        si.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        si.magFilter    = VK_FILTER_LINEAR;
        si.minFilter    = VK_FILTER_LINEAR;
        si.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        if (vkCreateSampler(m_gpu.device(), &si, nullptr, &m_bloomSampler) != VK_SUCCESS)
            throw std::runtime_error("failed to create bloom sampler!");

        // --- Compute: origen muestreado + destino como storage image ---------
        VkDescriptorSetLayoutBinding bloomBindings[2]{};
        bloomBindings[0].binding         = 0;
        bloomBindings[0].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bloomBindings[0].descriptorCount = 1;
        bloomBindings[0].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
        bloomBindings[1].binding         = 1;
        bloomBindings[1].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        bloomBindings[1].descriptorCount = 1;
        bloomBindings[1].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;

        VkDescriptorSetLayoutCreateInfo dsl{};
        dsl.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        dsl.bindingCount = 2;
        dsl.pBindings    = bloomBindings;
        if (vkCreateDescriptorSetLayout(m_gpu.device(), &dsl, nullptr, &m_bloomDescLayout) != VK_SUCCESS)
            throw std::runtime_error("failed to create bloom descriptor set layout!");

        // Un set por nivel y por sentido (bajada y subida), por frame en vuelo:
        // cada uno lleva un par origen/destino distinto y no se pueden reutilizar.
        const uint32_t bloomSets = MAX_FRAMES * BLOOM_MIPS * 2;
        VkDescriptorPoolSize bloomSizes[2]{};
        bloomSizes[0].type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bloomSizes[0].descriptorCount = bloomSets;
        bloomSizes[1].type            = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        bloomSizes[1].descriptorCount = bloomSets;

        VkDescriptorPoolCreateInfo dpi{};
        dpi.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        dpi.poolSizeCount = 2;
        dpi.pPoolSizes    = bloomSizes;
        dpi.maxSets       = bloomSets;
        if (vkCreateDescriptorPool(m_gpu.device(), &dpi, nullptr, &m_bloomDescPool) != VK_SUCCESS)
            throw std::runtime_error("failed to create bloom descriptor pool!");

        VkPushConstantRange bloomPcr{};
        bloomPcr.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        bloomPcr.offset     = 0;
        bloomPcr.size       = sizeof(BloomPush);

        VkPipelineLayoutCreateInfo pli{};
        pli.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pli.setLayoutCount         = 1;
        pli.pSetLayouts            = &m_bloomDescLayout;
        pli.pushConstantRangeCount = 1;
        pli.pPushConstantRanges    = &bloomPcr;
        if (vkCreatePipelineLayout(m_gpu.device(), &pli, nullptr, &m_bloomPipelineLayout) != VK_SUCCESS)
            throw std::runtime_error("failed to create bloom pipeline layout!");

        auto makeBloomPipeline = [&](const std::string& spv, VkPipeline& pipeline)
        {
            auto code   = loadShaderFile(spv);
            auto module = createShaderModule(code);

            VkComputePipelineCreateInfo ci{};
            ci.sType        = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
            ci.stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            ci.stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
            ci.stage.module = module;
            ci.stage.pName  = "main";
            ci.layout       = m_bloomPipelineLayout;
            if (vkCreateComputePipelines(m_gpu.device(), VK_NULL_HANDLE, 1, &ci, nullptr, &pipeline) != VK_SUCCESS)
                throw std::runtime_error("failed to create compute pipeline: " + spv);

            vkDestroyShaderModule(m_gpu.device(), module, nullptr);
        };

        makeBloomPipeline("shaders/bloom_down.comp.spv", m_bloomDownPipeline);
        makeBloomPipeline("shaders/bloom_up.comp.spv",   m_bloomUpPipeline);

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

        // --- Medicion del coste GPU -----------------------------------------
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(m_gpu.physicalDevice(), &props);
        m_timestampPeriod     = props.limits.timestampPeriod;
        m_timestampsSupported = props.limits.timestampComputeAndGraphics && m_timestampPeriod > 0.0f;
        if (m_timestampsSupported)
        {
            VkQueryPoolCreateInfo qpi{};
            qpi.sType      = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
            qpi.queryType  = VK_QUERY_TYPE_TIMESTAMP;
            qpi.queryCount = MAX_FRAMES * 2;
            if (vkCreateQueryPool(m_gpu.device(), &qpi, nullptr, &m_bloomQueryPool) != VK_SUCCESS)
                throw std::runtime_error("failed to create bloom query pool!");
        }

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
        // Cadena a media resolucion: el bloom es un desenfoque ancho, no aporta
        // nada resolverlo a tamano completo y cuesta 4x.
        uint32_t w = m_renderExtent.width  / 2;
        uint32_t h = m_renderExtent.height / 2;
        m_bloomMipCount = 0;
        for (uint32_t m = 0; m < BLOOM_MIPS && w >= 2 && h >= 2; m++)
        {
            m_bloomMipExtent[m] = { w, h };
            m_bloomMipCount++;
            w = (w / 2 < 1) ? 1u : w / 2;
            h = (h / 2 < 1) ? 1u : h / 2;
        }
        // Viewport diminuto (ventana casi cerrada): sin niveles no hay bloom que
        // calcular. recordBloomPass y la composicion lo comprueban.
        if (m_bloomMipCount == 0) return;

        for (int f = 0; f < MAX_FRAMES; f++)
        {
            // Inline y no m_res.createImage: esa fija mipLevels a 1.
            VkImageCreateInfo ci{};
            ci.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
            ci.imageType     = VK_IMAGE_TYPE_2D;
            ci.format        = kHdrFormat;
            ci.extent        = { m_bloomMipExtent[0].width, m_bloomMipExtent[0].height, 1 };
            ci.mipLevels     = m_bloomMipCount;
            ci.arrayLayers   = 1;
            ci.samples       = VK_SAMPLE_COUNT_1_BIT;
            ci.tiling        = VK_IMAGE_TILING_OPTIMAL;
            // TRANSFER_DST: con el efecto apagado la cadena se limpia a negro en
            // vez de calcularse, y la composicion la sigue muestreando.
            ci.usage         = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
            ci.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
            ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            if (vkCreateImage(m_gpu.device(), &ci, nullptr, &m_bloomImage[f]) != VK_SUCCESS)
                throw std::runtime_error("failed to create bloom image!");

            VkMemoryRequirements memReq;
            vkGetImageMemoryRequirements(m_gpu.device(), m_bloomImage[f], &memReq);
            VkMemoryAllocateInfo memAlloc{};
            memAlloc.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            memAlloc.allocationSize  = memReq.size;
            memAlloc.memoryTypeIndex = m_gpu.findMemoryType(memReq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            if (vkAllocateMemory(m_gpu.device(), &memAlloc, nullptr, &m_bloomMemory[f]) != VK_SUCCESS)
                throw std::runtime_error("failed to allocate bloom memory!");
            vkBindImageMemory(m_gpu.device(), m_bloomImage[f], m_bloomMemory[f], 0);

            for (uint32_t m = 0; m < m_bloomMipCount; m++)
            {
                VkImageViewCreateInfo vi{};
                vi.sType                           = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
                vi.image                           = m_bloomImage[f];
                vi.viewType                        = VK_IMAGE_VIEW_TYPE_2D;
                vi.format                          = kHdrFormat;
                vi.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
                vi.subresourceRange.baseMipLevel   = m;
                vi.subresourceRange.levelCount     = 1;
                vi.subresourceRange.baseArrayLayer = 0;
                vi.subresourceRange.layerCount     = 1;
                if (vkCreateImageView(m_gpu.device(), &vi, nullptr, &m_bloomMipView[f][m]) != VK_SUCCESS)
                    throw std::runtime_error("failed to create bloom mip view!");
            }

            // Recien creada: contenido indefinido y layout UNDEFINED. Con el bloom
            // encendido lo arregla recordBloomPass; apagado, el clear la deja en
            // negro y en GENERAL, que es lo que declara el set de composicion.
            m_bloomClearPending[f] = true;
        }

        // Los sets de la vez anterior apuntan a vistas ya destruidas: reset y no
        // free, igual que hace precomputeIbl al recargar el entorno.
        vkResetDescriptorPool(m_gpu.device(), m_bloomDescPool, 0);
        vkResetDescriptorPool(m_gpu.device(), m_compositeDescPool, 0);

        for (int f = 0; f < MAX_FRAMES; f++)
        {
            const uint32_t setCount = m_bloomMipCount * 2;
            std::vector<VkDescriptorSetLayout> layouts(setCount, m_bloomDescLayout);
            std::vector<VkDescriptorSet>       sets(setCount, VK_NULL_HANDLE);

            VkDescriptorSetAllocateInfo ai{};
            ai.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            ai.descriptorPool     = m_bloomDescPool;
            ai.descriptorSetCount = setCount;
            ai.pSetLayouts        = layouts.data();
            if (vkAllocateDescriptorSets(m_gpu.device(), &ai, sets.data()) != VK_SUCCESS)
                throw std::runtime_error("failed to allocate bloom descriptor sets!");

            // Los VkDescriptorImageInfo tienen que seguir vivos hasta el
            // vkUpdateDescriptorSets, asi que se dimensionan de golpe.
            std::vector<VkDescriptorImageInfo> srcInfos(setCount);
            std::vector<VkDescriptorImageInfo> dstInfos(setCount);
            std::vector<VkWriteDescriptorSet>  writes;
            writes.reserve(setCount * 2);

            auto pushPair = [&](uint32_t slot, VkDescriptorSet set,
                                VkImageView srcView, VkImageLayout srcLayout, VkImageView dstView)
            {
                srcInfos[slot].imageLayout = srcLayout;
                srcInfos[slot].imageView   = srcView;
                srcInfos[slot].sampler     = m_bloomSampler;
                dstInfos[slot].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
                dstInfos[slot].imageView   = dstView;

                VkWriteDescriptorSet src{};
                src.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                src.dstSet          = set;
                src.dstBinding      = 0;
                src.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                src.descriptorCount = 1;
                src.pImageInfo      = &srcInfos[slot];
                writes.push_back(src);

                VkWriteDescriptorSet dst = src;
                dst.dstBinding     = 1;
                dst.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
                dst.pImageInfo     = &dstInfos[slot];
                writes.push_back(dst);
            };

            for (uint32_t m = 0; m < m_bloomMipCount; m++)
            {
                // Bajada: el nivel 0 lee la escena HDR (que sale del render pass
                // en SHADER_READ_ONLY); los demas leen el mip anterior, que vive
                // en GENERAL toda la cadena.
                m_bloomDownSets[f][m] = sets[m];
                pushPair(m, sets[m],
                         m == 0 ? m_hdrView[f] : m_bloomMipView[f][m - 1],
                         m == 0 ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_GENERAL,
                         m_bloomMipView[f][m]);

                // Subida: lee el nivel m y acumula sobre el m-1. El nivel 0 no
                // tiene destino, asi que su set se queda sin usar.
                m_bloomUpSets[f][m] = VK_NULL_HANDLE;
                if (m > 0)
                {
                    const uint32_t slot = m_bloomMipCount + m;
                    m_bloomUpSets[f][m] = sets[slot];
                    pushPair(slot, sets[slot],
                             m_bloomMipView[f][m], VK_IMAGE_LAYOUT_GENERAL,
                             m_bloomMipView[f][m - 1]);
                }
            }
            vkUpdateDescriptorSets(m_gpu.device(), (uint32_t)writes.size(), writes.data(), 0, nullptr);

            // Set de composicion: escena HDR + mip 0 del bloom.
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
            compInfos[0].sampler     = m_bloomSampler;
            compInfos[1].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
            compInfos[1].imageView   = m_bloomMipView[f][0];
            compInfos[1].sampler     = m_bloomSampler;

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
        for (int f = 0; f < MAX_FRAMES; f++)
        {
            for (uint32_t m = 0; m < BLOOM_MIPS; m++)
            {
                if (m_bloomMipView[f][m])
                {
                    vkDestroyImageView(m_gpu.device(), m_bloomMipView[f][m], nullptr);
                    m_bloomMipView[f][m] = VK_NULL_HANDLE;
                }
                m_bloomDownSets[f][m] = VK_NULL_HANDLE;
                m_bloomUpSets[f][m]   = VK_NULL_HANDLE;
            }
            if (m_bloomImage[f])
            {
                vkDestroyImage(m_gpu.device(), m_bloomImage[f], nullptr);
                m_bloomImage[f] = VK_NULL_HANDLE;
            }
            if (m_bloomMemory[f])
            {
                vkFreeMemory(m_gpu.device(), m_bloomMemory[f], nullptr);
                m_bloomMemory[f] = VK_NULL_HANDLE;
            }
            m_compositeSets[f] = VK_NULL_HANDLE;
        }
        m_bloomMipCount = 0;
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
            for (int i = 0; i < MAX_FRAMES; i++) m_bloomClearPending[i] = true;
    }

    void Renderer::recordBloomClear(VkCommandBuffer cmd)
    {
        if (m_bloomMipCount == 0 || !m_bloomClearPending[m_currentFrame]) return;

        VkImageMemoryBarrier b{};
        b.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image               = m_bloomImage[m_currentFrame];
        b.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        b.subresourceRange.baseMipLevel   = 0;
        // Toda la cadena, no solo el mip 0: así los niveles quedan en GENERAL de
        // una vez y al reencender el bloom no hay layouts a medias.
        b.subresourceRange.levelCount     = m_bloomMipCount;
        b.subresourceRange.baseArrayLayer = 0;
        b.subresourceRange.layerCount     = 1;

        b.oldLayout     = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout     = VK_IMAGE_LAYOUT_GENERAL;
        b.srcAccessMask = 0;
        b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &b);

        VkClearColorValue black{};
        vkCmdClearColorImage(cmd, m_bloomImage[m_currentFrame], VK_IMAGE_LAYOUT_GENERAL,
                             &black, 1, &b.subresourceRange);

        b.oldLayout     = VK_IMAGE_LAYOUT_GENERAL;
        b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &b);

        m_bloomClearPending[m_currentFrame] = false;
    }

    void Renderer::recordBloomPass(VkCommandBuffer cmd)
    {
        if (m_bloomMipCount == 0) return;

        VkImageMemoryBarrier b{};
        b.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image               = m_bloomImage[m_currentFrame];
        b.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        b.subresourceRange.baseArrayLayer = 0;
        b.subresourceRange.layerCount     = 1;

        // Toda la cadena vive en GENERAL: es el unico layout que admite
        // imageStore y a la vez es valido para muestrear, asi que el ping-pong
        // de layouts entre pasos se reduce a barreras de memoria. Se entra desde
        // UNDEFINED porque el contenido del frame anterior no se reutiliza.
        b.oldLayout                     = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout                     = VK_IMAGE_LAYOUT_GENERAL;
        b.srcAccessMask                 = 0;
        b.dstAccessMask                 = VK_ACCESS_SHADER_WRITE_BIT;
        b.subresourceRange.baseMipLevel = 0;
        b.subresourceRange.levelCount   = m_bloomMipCount;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &b);

        // Barrera entre pasos: lo que acaba de escribir un dispatch lo lee el
        // siguiente. Se acota al mip implicado para no serializar de mas.
        auto writeToRead = [&](uint32_t mip)
        {
            b.oldLayout                     = VK_IMAGE_LAYOUT_GENERAL;
            b.newLayout                     = VK_IMAGE_LAYOUT_GENERAL;
            b.srcAccessMask                 = VK_ACCESS_SHADER_WRITE_BIT;
            // SHADER_WRITE ademas de READ: la subida hace imageLoad Y imageStore
            // sobre el mismo nivel que escribio la bajada, asi que sin esto
            // quedaria un write-after-write sin ordenar.
            b.dstAccessMask                 = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
            b.subresourceRange.baseMipLevel = mip;
            b.subresourceRange.levelCount   = 1;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                 0, 0, nullptr, 0, nullptr, 1, &b);
        };

        BloomPush push{};
        push.threshold = m_bloomThreshold;
        push.knee      = glm::max(m_bloomKnee, 1e-3f);
        push.radius    = 1.0f;

        // ── Bajada: HDR → mip 0 (con umbral) → mip 1 → ... ───────────────────
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_bloomDownPipeline);
        for (uint32_t m = 0; m < m_bloomMipCount; m++)
        {
            const VkExtent2D src = (m == 0) ? m_renderExtent : m_bloomMipExtent[m - 1];
            const VkExtent2D dst = m_bloomMipExtent[m];

            push.srcTexelX = 1.0f / (float)src.width;
            push.srcTexelY = 1.0f / (float)src.height;
            push.prefilter = (m == 0) ? 1 : 0;

            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_bloomPipelineLayout,
                                    0, 1, &m_bloomDownSets[m_currentFrame][m], 0, nullptr);
            vkCmdPushConstants(cmd, m_bloomPipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
            vkCmdDispatch(cmd, (dst.width + 7) / 8, (dst.height + 7) / 8, 1);

            if (m + 1 < m_bloomMipCount) writeToRead(m);
        }

        // ── Subida: cada mip se suma al de arriba con un tent 3x3 ────────────
        push.prefilter = 0;
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_bloomUpPipeline);
        for (uint32_t m = m_bloomMipCount - 1; m > 0; m--)
        {
            // El mip m acaba de escribirse (por la bajada si es el ultimo, por la
            // iteracion anterior de esta subida si no) y ahora se lee.
            writeToRead(m);

            const VkExtent2D src = m_bloomMipExtent[m];
            const VkExtent2D dst = m_bloomMipExtent[m - 1];
            push.srcTexelX = 1.0f / (float)src.width;
            push.srcTexelY = 1.0f / (float)src.height;

            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_bloomPipelineLayout,
                                    0, 1, &m_bloomUpSets[m_currentFrame][m], 0, nullptr);
            vkCmdPushConstants(cmd, m_bloomPipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
            vkCmdDispatch(cmd, (dst.width + 7) / 8, (dst.height + 7) / 8, 1);
        }

        // El mip 0 pasa a leerse desde el fragment shader de la composicion.
        b.oldLayout                     = VK_IMAGE_LAYOUT_GENERAL;
        b.newLayout                     = VK_IMAGE_LAYOUT_GENERAL;
        b.srcAccessMask                 = VK_ACCESS_SHADER_WRITE_BIT;
        b.dstAccessMask                 = VK_ACCESS_SHADER_READ_BIT;
        b.subresourceRange.baseMipLevel = 0;
        b.subresourceRange.levelCount   = 1;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &b);
    }

    // ── SSAO ────────────────────────────────────────────────────────────────
    void Renderer::createSsaoPipelines()
    {
        // NEAREST: ni D32_SFLOAT ni R32_SFLOAT tienen garantizado el filtrado
        // lineal, y los dos shaders muestrean a texel exacto. CLAMP_TO_EDGE para
        // que los taps del borde no traigan profundidad del lado opuesto.
        VkSamplerCreateInfo si{};
        si.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        si.magFilter    = VK_FILTER_NEAREST;
        si.minFilter    = VK_FILTER_NEAREST;
        si.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        if (vkCreateSampler(m_gpu.device(), &si, nullptr, &m_ssaoSampler) != VK_SUCCESS)
            throw std::runtime_error("failed to create ssao sampler!");

        // --- Render pass del depth pre-pass (solo profundidad) ---------------
        VkAttachmentDescription depthAtt{};
        depthAtt.format         = VK_FORMAT_D32_SFLOAT;
        depthAtt.samples        = VK_SAMPLE_COUNT_1_BIT;
        depthAtt.loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depthAtt.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
        depthAtt.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        depthAtt.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depthAtt.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
        // READ_ONLY: en cuanto acaba el pass, el compute del SSAO la muestrea.
        depthAtt.finalLayout    = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;

        VkAttachmentReference depthRef{};
        depthRef.attachment = 0;
        depthRef.layout     = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.pDepthStencilAttachment = &depthRef;

        VkSubpassDependency deps[2]{};
        // Entrada: el compute del frame anterior en este mismo slot pudo estar
        // leyendo esta imagen.
        deps[0].srcSubpass    = VK_SUBPASS_EXTERNAL;
        deps[0].dstSubpass    = 0;
        deps[0].srcStageMask  = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
        deps[0].dstStageMask  = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        deps[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        deps[0].dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        // Salida: ssao.comp muestrea la profundidad recién escrita.
        deps[1].srcSubpass    = 0;
        deps[1].dstSubpass    = VK_SUBPASS_EXTERNAL;
        deps[1].srcStageMask  = VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        deps[1].dstStageMask  = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
        deps[1].srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        deps[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

        VkRenderPassCreateInfo rpInfo{};
        rpInfo.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        rpInfo.attachmentCount = 1;
        rpInfo.pAttachments    = &depthAtt;
        rpInfo.subpassCount    = 1;
        rpInfo.pSubpasses      = &subpass;
        rpInfo.dependencyCount = 2;
        rpInfo.pDependencies   = deps;
        if (vkCreateRenderPass(m_gpu.device(), &rpInfo, nullptr, &m_ssaoDepthRenderPass) != VK_SUCCESS)
            throw std::runtime_error("failed to create ssao depth render pass!");

        // --- Pipeline del pre-pass (vertex-only, como el de sombras) ---------
        auto vertCode = loadShaderFile("shaders/depth_prepass.vert.spv");
        VkShaderModule vertModule = createShaderModule(vertCode);

        VkPipelineShaderStageCreateInfo vertStage{};
        vertStage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        vertStage.stage  = VK_SHADER_STAGE_VERTEX_BIT;
        vertStage.module = vertModule;
        vertStage.pName  = "main";

        VkVertexInputBindingDescription bindingDesc{};
        bindingDesc.binding   = 0;
        bindingDesc.stride    = sizeof(Vertex);
        bindingDesc.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

        VkVertexInputAttributeDescription attrDesc{};
        attrDesc.binding  = 0;
        attrDesc.location = 0;
        attrDesc.format   = VK_FORMAT_R32G32B32_SFLOAT;
        attrDesc.offset   = offsetof(Vertex, pos);

        VkPipelineVertexInputStateCreateInfo vertexInput{};
        vertexInput.sType                           = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vertexInput.vertexBindingDescriptionCount   = 1;
        vertexInput.pVertexBindingDescriptions      = &bindingDesc;
        vertexInput.vertexAttributeDescriptionCount = 1;
        vertexInput.pVertexAttributeDescriptions    = &attrDesc;

        VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
        inputAssembly.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

        VkPipelineViewportStateCreateInfo viewportState{};
        viewportState.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        viewportState.viewportCount = 1;
        viewportState.scissorCount  = 1;

        VkPipelineRasterizationStateCreateInfo rasterizer{};
        rasterizer.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
        rasterizer.cullMode    = VK_CULL_MODE_NONE;
        rasterizer.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rasterizer.lineWidth   = 1.0f;
        // Sin depthBias, al reves que el pass de sombras: esta profundidad no se
        // compara contra nada, se reconstruye a posicion. Un sesgo aqui movería
        // la geometría en Z y el AO saldría despegado del contacto.

        VkPipelineMultisampleStateCreateInfo multisampling{};
        multisampling.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        VkPipelineDepthStencilStateCreateInfo depthStencil{};
        depthStencil.sType            = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        depthStencil.depthTestEnable  = VK_TRUE;
        depthStencil.depthWriteEnable = VK_TRUE;
        depthStencil.depthCompareOp   = VK_COMPARE_OP_LESS_OR_EQUAL;

        VkPipelineColorBlendStateCreateInfo colorBlend{};
        colorBlend.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        colorBlend.attachmentCount = 0;

        VkDynamicState dynStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo dynamicState{};
        dynamicState.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dynamicState.dynamicStateCount = 2;
        dynamicState.pDynamicStates    = dynStates;

        VkGraphicsPipelineCreateInfo pipelineInfo{};
        pipelineInfo.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        pipelineInfo.stageCount          = 1;
        pipelineInfo.pStages             = &vertStage;
        pipelineInfo.pVertexInputState   = &vertexInput;
        pipelineInfo.pInputAssemblyState = &inputAssembly;
        pipelineInfo.pViewportState      = &viewportState;
        pipelineInfo.pRasterizationState = &rasterizer;
        pipelineInfo.pMultisampleState   = &multisampling;
        pipelineInfo.pDepthStencilState  = &depthStencil;
        pipelineInfo.pColorBlendState    = &colorBlend;
        pipelineInfo.pDynamicState       = &dynamicState;
        // Prestado del pass de sombras: mismos dos sets (objeto + SSBO de
        // instancias) y mismo rango de push constants, que este shader no usa.
        pipelineInfo.layout              = m_shadowPipelineLayout;
        pipelineInfo.renderPass          = m_ssaoDepthRenderPass;
        if (vkCreateGraphicsPipelines(m_gpu.device(), VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_ssaoDepthPipeline) != VK_SUCCESS)
            throw std::runtime_error("failed to create ssao depth pipeline!");
        vkDestroyShaderModule(m_gpu.device(), vertModule, nullptr);

        // --- Compute: origen muestreado + destino como storage image ---------
        VkDescriptorSetLayoutBinding ssaoBindings[2]{};
        ssaoBindings[0].binding         = 0;
        ssaoBindings[0].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        ssaoBindings[0].descriptorCount = 1;
        ssaoBindings[0].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
        ssaoBindings[1].binding         = 1;
        ssaoBindings[1].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        ssaoBindings[1].descriptorCount = 1;
        ssaoBindings[1].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;

        VkDescriptorSetLayoutCreateInfo dsl{};
        dsl.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        dsl.bindingCount = 2;
        dsl.pBindings    = ssaoBindings;
        if (vkCreateDescriptorSetLayout(m_gpu.device(), &dsl, nullptr, &m_ssaoDescLayout) != VK_SUCCESS)
            throw std::runtime_error("failed to create ssao descriptor set layout!");

        // Dos sets por frame: oclusion (depth → AO) y blur (AO → AO suavizado).
        const uint32_t ssaoSets = MAX_FRAMES * 2;
        VkDescriptorPoolSize ssaoSizes[2]{};
        ssaoSizes[0].type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        ssaoSizes[0].descriptorCount = ssaoSets;
        ssaoSizes[1].type            = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        ssaoSizes[1].descriptorCount = ssaoSets;

        VkDescriptorPoolCreateInfo dpi{};
        dpi.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        dpi.poolSizeCount = 2;
        dpi.pPoolSizes    = ssaoSizes;
        dpi.maxSets       = ssaoSets;
        if (vkCreateDescriptorPool(m_gpu.device(), &dpi, nullptr, &m_ssaoDescPool) != VK_SUCCESS)
            throw std::runtime_error("failed to create ssao descriptor pool!");

        VkPushConstantRange pcr{};
        pcr.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        pcr.offset     = 0;
        pcr.size       = sizeof(SsaoPush);

        VkPipelineLayoutCreateInfo pli{};
        pli.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pli.setLayoutCount         = 1;
        pli.pSetLayouts            = &m_ssaoDescLayout;
        pli.pushConstantRangeCount = 1;
        pli.pPushConstantRanges    = &pcr;
        if (vkCreatePipelineLayout(m_gpu.device(), &pli, nullptr, &m_ssaoPipelineLayout) != VK_SUCCESS)
            throw std::runtime_error("failed to create ssao pipeline layout!");

        auto makeSsaoPipeline = [&](const std::string& spv, VkPipeline& pipeline)
        {
            auto code   = loadShaderFile(spv);
            auto module = createShaderModule(code);

            VkComputePipelineCreateInfo ci{};
            ci.sType        = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
            ci.stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            ci.stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
            ci.stage.module = module;
            ci.stage.pName  = "main";
            ci.layout       = m_ssaoPipelineLayout;
            if (vkCreateComputePipelines(m_gpu.device(), VK_NULL_HANDLE, 1, &ci, nullptr, &pipeline) != VK_SUCCESS)
                throw std::runtime_error("failed to create compute pipeline: " + spv);

            vkDestroyShaderModule(m_gpu.device(), module, nullptr);
        };

        makeSsaoPipeline("shaders/ssao.comp.spv",      m_ssaoPipeline);
        makeSsaoPipeline("shaders/ssao_blur.comp.spv", m_ssaoBlurPipeline);

        // Queries propias: m_timestampsSupported y m_timestampPeriod ya los
        // resolvio createBloomPipelines, que corre antes.
        if (m_timestampsSupported)
        {
            VkQueryPoolCreateInfo qpi{};
            qpi.sType      = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
            qpi.queryType  = VK_QUERY_TYPE_TIMESTAMP;
            qpi.queryCount = MAX_FRAMES * 2;
            if (vkCreateQueryPool(m_gpu.device(), &qpi, nullptr, &m_ssaoQueryPool) != VK_SUCCESS)
                throw std::runtime_error("failed to create ssao query pool!");
        }

        printf("ssao pipelines OK\n"); fflush(stdout);
    }

    void Renderer::createSsaoImages()
    {
        for (int f = 0; f < MAX_FRAMES; f++)
        {
            // Depth del pre-pass. SAMPLED ademas de ATTACHMENT: lo muestrea
            // ssao.comp.
            m_res.createImage(
                m_renderExtent.width, m_renderExtent.height,
                VK_FORMAT_D32_SFLOAT,
                VK_IMAGE_TILING_OPTIMAL,
                VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                m_ssaoDepthImage[f], m_ssaoDepthMemory[f]);

            // Inline y no createTextureImageView: esa fija el aspecto a COLOR.
            VkImageViewCreateInfo dvi{};
            dvi.sType                           = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            dvi.image                           = m_ssaoDepthImage[f];
            dvi.viewType                        = VK_IMAGE_VIEW_TYPE_2D;
            dvi.format                          = VK_FORMAT_D32_SFLOAT;
            dvi.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_DEPTH_BIT;
            dvi.subresourceRange.levelCount     = 1;
            dvi.subresourceRange.layerCount     = 1;
            if (vkCreateImageView(m_gpu.device(), &dvi, nullptr, &m_ssaoDepthView[f]) != VK_SUCCESS)
                throw std::runtime_error("failed to create ssao depth view!");

            VkFramebufferCreateInfo fbInfo{};
            fbInfo.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            fbInfo.renderPass      = m_ssaoDepthRenderPass;
            fbInfo.attachmentCount = 1;
            fbInfo.pAttachments    = &m_ssaoDepthView[f];
            fbInfo.width           = m_renderExtent.width;
            fbInfo.height          = m_renderExtent.height;
            fbInfo.layers          = 1;
            if (vkCreateFramebuffer(m_gpu.device(), &fbInfo, nullptr, &m_ssaoDepthFb[f]) != VK_SUCCESS)
                throw std::runtime_error("failed to create ssao depth framebuffer!");

            // AO crudo y AO emborronado. TRANSFER_DST en el segundo: con el
            // efecto apagado se limpia a 1.0 en vez de calcularse.
            m_res.createImage(
                m_renderExtent.width, m_renderExtent.height,
                kSsaoFormat, VK_IMAGE_TILING_OPTIMAL,
                VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                m_ssaoImage[f], m_ssaoMemory[f]);
            m_res.createTextureImageView(m_ssaoImage[f], m_ssaoView[f], kSsaoFormat);

            m_res.createImage(
                m_renderExtent.width, m_renderExtent.height,
                kSsaoFormat, VK_IMAGE_TILING_OPTIMAL,
                VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                m_ssaoBlurImage[f], m_ssaoBlurMemory[f]);
            m_res.createTextureImageView(m_ssaoBlurImage[f], m_ssaoBlurView[f], kSsaoFormat);

            // Recien creada: contenido indefinido y layout UNDEFINED. El clear la
            // deja en 1.0 y en GENERAL, que es lo que declara el binding 7.
            m_ssaoClearPending[f] = true;
        }

        // Los sets de la vez anterior apuntan a vistas ya destruidas: reset y no
        // free, igual que en el bloom.
        vkResetDescriptorPool(m_gpu.device(), m_ssaoDescPool, 0);

        for (int f = 0; f < MAX_FRAMES; f++)
        {
            VkDescriptorSetLayout layouts[2] = { m_ssaoDescLayout, m_ssaoDescLayout };
            VkDescriptorSet       sets[2]    = {};

            VkDescriptorSetAllocateInfo ai{};
            ai.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            ai.descriptorPool     = m_ssaoDescPool;
            ai.descriptorSetCount = 2;
            ai.pSetLayouts        = layouts;
            if (vkAllocateDescriptorSets(m_gpu.device(), &ai, sets) != VK_SUCCESS)
                throw std::runtime_error("failed to allocate ssao descriptor sets!");

            m_ssaoSets[f]     = sets[0];
            m_ssaoBlurSets[f] = sets[1];

            VkDescriptorImageInfo infos[4]{};
            // Oclusion: lee el depth del pre-pass, escribe el AO crudo.
            infos[0].imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
            infos[0].imageView   = m_ssaoDepthView[f];
            infos[0].sampler     = m_ssaoSampler;
            infos[1].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
            infos[1].imageView   = m_ssaoView[f];
            // Blur: lee el AO crudo, escribe el que consume pbr.frag.
            infos[2].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
            infos[2].imageView   = m_ssaoView[f];
            infos[2].sampler     = m_ssaoSampler;
            infos[3].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
            infos[3].imageView   = m_ssaoBlurView[f];

            VkWriteDescriptorSet writes[4]{};
            for (int i = 0; i < 4; i++)
            {
                writes[i].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                writes[i].dstSet          = (i < 2) ? m_ssaoSets[f] : m_ssaoBlurSets[f];
                writes[i].dstBinding      = (uint32_t)(i % 2);
                writes[i].descriptorType  = (i % 2 == 0) ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
                                                         : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
                writes[i].descriptorCount = 1;
                writes[i].pImageInfo      = &infos[i];
            }
            vkUpdateDescriptorSets(m_gpu.device(), 4, writes, 0, nullptr);
        }

        // Los sets por objeto guardan la vista vieja: hay que repasarlos. En el
        // arranque no hay ninguno todavia y el bucle no hace nada.
        refreshSsaoDescriptors();
    }

    void Renderer::destroySsaoImages()
    {
        for (int f = 0; f < MAX_FRAMES; f++)
        {
            if (m_ssaoDepthFb[f])
            {
                vkDestroyFramebuffer(m_gpu.device(), m_ssaoDepthFb[f], nullptr);
                m_ssaoDepthFb[f] = VK_NULL_HANDLE;
            }
            if (m_ssaoDepthView[f])
            {
                vkDestroyImageView(m_gpu.device(), m_ssaoDepthView[f], nullptr);
                m_ssaoDepthView[f] = VK_NULL_HANDLE;
            }
            if (m_ssaoDepthImage[f])
            {
                vkDestroyImage(m_gpu.device(), m_ssaoDepthImage[f], nullptr);
                m_ssaoDepthImage[f] = VK_NULL_HANDLE;
            }
            if (m_ssaoDepthMemory[f])
            {
                vkFreeMemory(m_gpu.device(), m_ssaoDepthMemory[f], nullptr);
                m_ssaoDepthMemory[f] = VK_NULL_HANDLE;
            }
            if (m_ssaoView[f])
            {
                vkDestroyImageView(m_gpu.device(), m_ssaoView[f], nullptr);
                m_ssaoView[f] = VK_NULL_HANDLE;
            }
            if (m_ssaoImage[f])
            {
                vkDestroyImage(m_gpu.device(), m_ssaoImage[f], nullptr);
                m_ssaoImage[f] = VK_NULL_HANDLE;
            }
            if (m_ssaoMemory[f])
            {
                vkFreeMemory(m_gpu.device(), m_ssaoMemory[f], nullptr);
                m_ssaoMemory[f] = VK_NULL_HANDLE;
            }
            if (m_ssaoBlurView[f])
            {
                vkDestroyImageView(m_gpu.device(), m_ssaoBlurView[f], nullptr);
                m_ssaoBlurView[f] = VK_NULL_HANDLE;
            }
            if (m_ssaoBlurImage[f])
            {
                vkDestroyImage(m_gpu.device(), m_ssaoBlurImage[f], nullptr);
                m_ssaoBlurImage[f] = VK_NULL_HANDLE;
            }
            if (m_ssaoBlurMemory[f])
            {
                vkFreeMemory(m_gpu.device(), m_ssaoBlurMemory[f], nullptr);
                m_ssaoBlurMemory[f] = VK_NULL_HANDLE;
            }
            m_ssaoSets[f]     = VK_NULL_HANDLE;
            m_ssaoBlurSets[f] = VK_NULL_HANDLE;
        }
    }

    void Renderer::recordSsaoPass(VkCommandBuffer cmd, const Frustum& camFrustum, const glm::mat4& proj)
    {
        // Viewport degenerado o recursos aun sin crear: nada que hacer.
        if (m_ssaoBlurImage[m_currentFrame] == VK_NULL_HANDLE) return;

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
        const bool ssrNeedsDepth = ssrActive() || m_aaActiveMode == AaMode::Taa ||
                                   m_fpActiveMode == FpMode::Tiled || m_fogEnabled ||
                                   m_motionBlurPass.active(motionBlurCtx());
        m_ssrStampedPrepass = false;

        VkImageMemoryBarrier b{};
        b.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        b.subresourceRange.baseMipLevel   = 0;
        b.subresourceRange.levelCount     = 1;
        b.subresourceRange.baseArrayLayer = 0;
        b.subresourceRange.layerCount     = 1;

        if (!ssaoEnabled())
        {
            m_ssaoGpuMs = 0.0f;
            // Apagado: ni oclusión ni blur. Solo queda dejar el mapa en la
            // identidad, y eso pasa UNA vez por imagen (al crearla y al apagar el
            // efecto), no cada frame.
            if (m_ssaoClearPending[m_currentFrame])
            {
                b.image         = m_ssaoBlurImage[m_currentFrame];
                b.oldLayout     = VK_IMAGE_LAYOUT_UNDEFINED;
                b.newLayout     = VK_IMAGE_LAYOUT_GENERAL;
                b.srcAccessMask = 0;
                b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                     0, 0, nullptr, 0, nullptr, 1, &b);

                VkClearColorValue white{};
                white.float32[0] = 1.0f;
                vkCmdClearColorImage(cmd, m_ssaoBlurImage[m_currentFrame], VK_IMAGE_LAYOUT_GENERAL,
                                     &white, 1, &b.subresourceRange);

                b.oldLayout     = VK_IMAGE_LAYOUT_GENERAL;
                b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
                vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                     0, 0, nullptr, 0, nullptr, 1, &b);

                m_ssaoClearPending[m_currentFrame] = false;
            }
            // Y si tampoco el SSR necesita la profundidad, aquí acaba el frame
            // para este pass: cero trabajo grabado.
            if (!ssrNeedsDepth) return;
        }

        // Timestamps del slot: se leen los de hace dos frames, cuya fence ya
        // esperó drawFrame, así que no bloquean a nadie.
        if (ssaoEnabled() && m_timestampsSupported && m_ssaoQueryPending[m_currentFrame])
        {
            uint64_t stamps[2] = {};
            if (vkGetQueryPoolResults(m_gpu.device(), m_ssaoQueryPool, m_currentFrame * 2, 2,
                                      sizeof(stamps), stamps, sizeof(uint64_t),
                                      VK_QUERY_RESULT_64_BIT) == VK_SUCCESS)
            {
                m_ssaoGpuMs = (float)((double)(stamps[1] - stamps[0]) * m_timestampPeriod * 1e-6);
                if (++m_ssaoMeasuredFrames == 300)
                {
                    printf("ssao (depth pre-pass + 2 dispatches): %.3f ms (%ux%u)\n",
                           m_ssaoGpuMs, m_swapChainExtent.width, m_swapChainExtent.height);
                    fflush(stdout);
                }
            }
        }
        if (ssaoEnabled() && m_timestampsSupported)
        {
            vkCmdResetQueryPool(cmd, m_ssaoQueryPool, m_currentFrame * 2, 2);
            vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, m_ssaoQueryPool, m_currentFrame * 2);
            m_ssaoQueryPending[m_currentFrame] = true;
        }

        // Queries del SSR: se resetean las CUATRO aquí, que es lo primero suyo
        // que se graba en el frame, y el par [0,1] acota el pre-pass. Con el SSAO
        // encendido ese coste ya lo mide su propio par y recordSsrPass no lo
        // vuelve a sumar; el par se escribe igualmente para que la lectura de los
        // cuatro nunca dé NOT_READY.
        if (ssrNeedsDepth && m_timestampsSupported)
        {
            vkCmdResetQueryPool(cmd, m_ssrQueryPool, m_currentFrame * 4, 4);
            vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, m_ssrQueryPool, m_currentFrame * 4);
            m_ssrStampedPrepass = true;
        }

        // ── Depth pre-pass: la escena entera, solo profundidad ───────────────
        {
            VkClearValue clearDepth{};
            clearDepth.depthStencil = { 1.0f, 0 };

            VkRenderPassBeginInfo rpInfo{};
            rpInfo.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
            rpInfo.renderPass        = m_ssaoDepthRenderPass;
            rpInfo.framebuffer       = m_ssaoDepthFb[m_currentFrame];
            rpInfo.renderArea.extent = m_renderExtent;
            rpInfo.renderArea.offset = { 0, 0 };
            rpInfo.clearValueCount   = 1;
            rpInfo.pClearValues      = &clearDepth;

            vkCmdBeginRenderPass(cmd, &rpInfo, VK_SUBPASS_CONTENTS_INLINE);

            VkViewport vp{};
            vp.width    = (float)m_renderExtent.width;
            vp.height   = (float)m_renderExtent.height;
            vp.minDepth = 0.0f;
            vp.maxDepth = 1.0f;
            vkCmdSetViewport(cmd, 0, 1, &vp);

            VkRect2D sc{};
            sc.extent = m_renderExtent;
            vkCmdSetScissor(cmd, 0, 1, &sc);

            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_ssaoDepthPipeline);

            // Mismas guardas y mismo frustum que el pass de escena: si aquí
            // entrara algo que allí no se dibuja, el AO oscurecería contra
            // geometría invisible. Los skinned no entran, igual que en el pass de
            // sombras: reciben AO pero no lo proyectan.
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

            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_shadowPipelineLayout,
                1, 1, &m_instanceDescSets[m_currentFrame], 0, nullptr);

            for (const InstanceBatch& batch : m_instanceBatches)
            {
                const SharedGpuMesh* gpu = m_sharedMeshes.get(batch.sharedIndex);
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_shadowPipelineLayout,
                    0, 1, &gpu->descriptorSets[m_currentFrame], 0, nullptr);

                VkBuffer vb[] = { gpu->vertexBuffer };
                VkDeviceSize offsets[] = { 0 };
                vkCmdBindVertexBuffers(cmd, 0, 1, vb, offsets);
                vkCmdBindIndexBuffer(cmd, gpu->indexBuffer, 0, VK_INDEX_TYPE_UINT32);
                vkCmdDrawIndexed(cmd, gpu->indexCount, batch.instanceCount, 0, 0, batch.firstInstance);
            }

            vkCmdEndRenderPass(cmd);
        }

        if (m_ssrStampedPrepass)
            vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, m_ssrQueryPool, m_currentFrame * 4 + 1);

        // El SSR solo quería la profundidad: sin SSAO no hay oclusión que
        // calcular ni mapa que escribir.
        if (!ssaoEnabled()) return;

        // ── Oclusión + blur ──────────────────────────────────────────────────
        SsaoPush push{};
        // Los cuatro coeficientes de la proyección EFECTIVA del frame, con el
        // Y-flip de Vulkan ya dentro: es la misma con la que se acaba de grabar
        // el depth, así que reconstruir y reproyectar es consistente.
        push.projP00   = proj[0][0];
        push.projP11   = proj[1][1];
        push.projP22   = proj[2][2];
        push.projP32   = proj[3][2];
        push.invResX   = 1.0f / (float)m_renderExtent.width;
        push.invResY   = 1.0f / (float)m_renderExtent.height;
        push.radius    = m_ssaoRadius;
        push.bias      = m_ssaoBias;
        push.intensity = m_ssaoIntensity;
        push.power     = m_ssaoPower;

        // Las dos imágenes entran desde UNDEFINED: se reescriben enteras y el
        // contenido del frame anterior no se reutiliza. GENERAL para las dos,
        // que es el único layout válido a la vez para imageStore y para
        // muestrear, igual que en la cadena del bloom.
        VkImageMemoryBarrier toGeneral[2] = { b, b };
        toGeneral[0].image         = m_ssaoImage[m_currentFrame];
        toGeneral[1].image         = m_ssaoBlurImage[m_currentFrame];
        for (int i = 0; i < 2; i++)
        {
            toGeneral[i].oldLayout     = VK_IMAGE_LAYOUT_UNDEFINED;
            toGeneral[i].newLayout     = VK_IMAGE_LAYOUT_GENERAL;
            toGeneral[i].srcAccessMask = 0;
            toGeneral[i].dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        }
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             0, 0, nullptr, 0, nullptr, 2, toGeneral);

        const uint32_t gx = (m_renderExtent.width  + 7) / 8;
        const uint32_t gy = (m_renderExtent.height + 7) / 8;

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_ssaoPipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_ssaoPipelineLayout,
                                0, 1, &m_ssaoSets[m_currentFrame], 0, nullptr);
        vkCmdPushConstants(cmd, m_ssaoPipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
        vkCmdDispatch(cmd, gx, gy, 1);

        // Lo que acaba de escribir la oclusión lo lee el blur.
        b.image         = m_ssaoImage[m_currentFrame];
        b.oldLayout     = VK_IMAGE_LAYOUT_GENERAL;
        b.newLayout     = VK_IMAGE_LAYOUT_GENERAL;
        b.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &b);

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_ssaoBlurPipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_ssaoPipelineLayout,
                                0, 1, &m_ssaoBlurSets[m_currentFrame], 0, nullptr);
        vkCmdPushConstants(cmd, m_ssaoPipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
        vkCmdDispatch(cmd, gx, gy, 1);

        // Y el resultado lo lee pbr.frag en el pass de escena.
        b.image         = m_ssaoBlurImage[m_currentFrame];
        b.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &b);

        if (m_timestampsSupported)
            vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, m_ssaoQueryPool, m_currentFrame * 2 + 1);
    }

    // ── Forward+ ────────────────────────────────────────────────────────────
    void Renderer::fpGridDims(FpMode mode, uint32_t& gridX, uint32_t& gridY,
                              uint32_t& gridZ, uint32_t& tileSize) const
    {
        // Con m_renderExtent y NO con m_swapChainExtent: con SSAA el render es
        // mayor que la ventana, y dimensionar con el de la ventana dejaria a
        // pbr.frag leyendo celdas fuera del buffer sin que la validacion diga
        // nada (gl_FragCoord va en pixeles del target).
        if (mode == FpMode::Clustered)
        {
            tileSize = kFpClusterTile;
            gridZ    = kFpClusterSlices;
        }
        else
        {
            tileSize = kFpTileSize;
            gridZ    = 1;
        }
        gridX = (m_renderExtent.width  + tileSize - 1) / tileSize;
        gridY = (m_renderExtent.height + tileSize - 1) / tileSize;
    }

    void Renderer::createFpPipelines()
    {
        // Seis bindings. Los cuatro primeros los ve tambien pbr.frag (set 2); la
        // profundidad y los contadores son solo del compute, y que el fragment
        // shader no los declare es legal.
        VkDescriptorSetLayoutBinding bindings[6]{};
        for (uint32_t i = 0; i < 6; i++)
        {
            bindings[i].binding         = i;
            bindings[i].descriptorType  = (i == 4) ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
                                                   : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            bindings[i].descriptorCount = 1;
            bindings[i].stageFlags      = (i < 4) ? (VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT)
                                                  : VK_SHADER_STAGE_COMPUTE_BIT;
        }

        VkDescriptorSetLayoutCreateInfo dsl{};
        dsl.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        dsl.bindingCount = 6;
        dsl.pBindings    = bindings;
        if (vkCreateDescriptorSetLayout(m_gpu.device(), &dsl, nullptr, &m_fpDescLayout) != VK_SUCCESS)
            throw std::runtime_error("failed to create forward+ descriptor set layout!");

        VkDescriptorPoolSize sizes[2]{};
        sizes[0].type            = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        sizes[0].descriptorCount = MAX_FRAMES * 5;
        sizes[1].type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        sizes[1].descriptorCount = MAX_FRAMES;

        VkDescriptorPoolCreateInfo dpi{};
        dpi.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        dpi.poolSizeCount = 2;
        dpi.pPoolSizes    = sizes;
        dpi.maxSets       = MAX_FRAMES;
        if (vkCreateDescriptorPool(m_gpu.device(), &dpi, nullptr, &m_fpDescPool) != VK_SUCCESS)
            throw std::runtime_error("failed to create forward+ descriptor pool!");

        // El pipeline de culling declara el set en el indice 0; el de escena lo
        // declara en el 2. Es el mismo VkDescriptorSet: un set encaja en
        // cualquier indice mientras el VkDescriptorSetLayout coincida.
        VkPushConstantRange pcr{};
        pcr.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        pcr.offset     = 0;
        pcr.size       = sizeof(FpPush);

        VkPipelineLayoutCreateInfo pli{};
        pli.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pli.setLayoutCount         = 1;
        pli.pSetLayouts            = &m_fpDescLayout;
        pli.pushConstantRangeCount = 1;
        pli.pPushConstantRanges    = &pcr;
        if (vkCreatePipelineLayout(m_gpu.device(), &pli, nullptr, &m_fpPipelineLayout) != VK_SUCCESS)
            throw std::runtime_error("failed to create forward+ pipeline layout!");

        auto makePipeline = [&](const std::string& spv, VkPipeline& pipeline)
        {
            auto code   = loadShaderFile(spv);
            auto module = createShaderModule(code);

            VkComputePipelineCreateInfo ci{};
            ci.sType        = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
            ci.stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            ci.stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
            ci.stage.module = module;
            ci.stage.pName  = "main";
            ci.layout       = m_fpPipelineLayout;
            if (vkCreateComputePipelines(m_gpu.device(), VK_NULL_HANDLE, 1, &ci, nullptr, &pipeline) != VK_SUCCESS)
                throw std::runtime_error("failed to create compute pipeline: " + spv);

            vkDestroyShaderModule(m_gpu.device(), module, nullptr);
        };

        makePipeline("shaders/light_cull_tiled.comp.spv",     m_fpTiledPipeline);
        makePipeline("shaders/light_cull_clustered.comp.spv", m_fpClusteredPipeline);

        // Parametros, luces y contadores: no dependen del tamano, viven todo el
        // proceso y se escriben desde la CPU cada frame (mapeo persistente, igual
        // que el UBO). Los contadores ademas se LEEN: los escribe la GPU con
        // atomicos y la CPU los recoge dos frames despues.
        const VkMemoryPropertyFlags hostFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        for (int f = 0; f < MAX_FRAMES; f++)
        {
            m_res.createBuffer(sizeof(FpParamsGpu), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, hostFlags,
                               m_fpParamsBuffer[f], m_fpParamsMemory[f]);
            vkMapMemory(m_gpu.device(), m_fpParamsMemory[f], 0, sizeof(FpParamsGpu), 0, &m_fpParamsMapped[f]);
            memset(m_fpParamsMapped[f], 0, sizeof(FpParamsGpu));

            const VkDeviceSize lightSize = sizeof(FpLightGpu) * kFpMaxLights;
            m_res.createBuffer(lightSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, hostFlags,
                               m_fpLightBuffer[f], m_fpLightMemory[f]);
            vkMapMemory(m_gpu.device(), m_fpLightMemory[f], 0, lightSize, 0, &m_fpLightMapped[f]);
            memset(m_fpLightMapped[f], 0, (size_t)lightSize);

            // 4 uint: [0] suma de luces asignadas, [1] celdas no vacias,
            // [2] celdas desbordadas, [3] sin usar (alineacion).
            m_res.createBuffer(sizeof(uint32_t) * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, hostFlags,
                               m_fpStatsBuffer[f], m_fpStatsMemory[f]);
            vkMapMemory(m_gpu.device(), m_fpStatsMemory[f], 0, sizeof(uint32_t) * 4, 0, &m_fpStatsMapped[f]);
            memset(m_fpStatsMapped[f], 0, sizeof(uint32_t) * 4);
        }

        // Queries propias: mezclarlas con las del SSAO o las del AA juntaria dos
        // medidas. Este pass corre ANTES que createBloomPipelines (el layout del
        // pipeline de escena necesita el set de aqui), asi que el soporte de
        // timestamps se resuelve aqui mismo en vez de heredarlo; son propiedades
        // del device y el bloom volvera a leer exactamente lo mismo.
        VkPhysicalDeviceProperties tsProps{};
        vkGetPhysicalDeviceProperties(m_gpu.physicalDevice(), &tsProps);
        m_timestampPeriod     = tsProps.limits.timestampPeriod;
        m_timestampsSupported = tsProps.limits.timestampComputeAndGraphics && m_timestampPeriod > 0.0f;
        if (m_timestampsSupported)
        {
            VkQueryPoolCreateInfo qpi{};
            qpi.sType      = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
            qpi.queryType  = VK_QUERY_TYPE_TIMESTAMP;
            qpi.queryCount = MAX_FRAMES * 2;
            if (vkCreateQueryPool(m_gpu.device(), &qpi, nullptr, &m_fpQueryPool) != VK_SUCCESS)
                throw std::runtime_error("failed to create forward+ query pool!");
        }

        printf("forward+ pipelines OK\n"); fflush(stdout);
    }

    void Renderer::createFpBuffers()
    {
        // Al MAYOR de las dos rejillas: asi cambiar de modo en caliente no
        // recrea nada y no puede quedar un frame grabado con los buffers del modo
        // anterior. La diferencia de memoria entre una y otra es despreciable al
        // lado de tener dos juegos de buffers.
        uint32_t gx = 0, gy = 0, gz = 0, ts = 0;
        fpGridDims(FpMode::Tiled, gx, gy, gz, ts);
        uint32_t maxCells = gx * gy * gz;
        fpGridDims(FpMode::Clustered, gx, gy, gz, ts);
        maxCells = std::max(maxCells, gx * gy * gz);
        // Viewport degenerado: nada que dimensionar. El resto del frame ya se
        // salta el pass entero.
        if (maxCells == 0) return;

        for (int f = 0; f < MAX_FRAMES; f++)
        {
            m_res.createBuffer((VkDeviceSize)maxCells * sizeof(uint32_t) * 2,
                               VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                               m_fpGridBuffer[f], m_fpGridMemory[f]);
            m_res.createBuffer((VkDeviceSize)maxCells * kFpMaxPerCell * sizeof(uint32_t),
                               VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                               m_fpIndexBuffer[f], m_fpIndexMemory[f]);
        }

        // Los sets de la vez anterior apuntan a buffers ya destruidos: reset y no
        // free, igual que en el bloom y en el SSAO.
        vkResetDescriptorPool(m_gpu.device(), m_fpDescPool, 0);

        for (int f = 0; f < MAX_FRAMES; f++)
        {
            VkDescriptorSetAllocateInfo ai{};
            ai.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            ai.descriptorPool     = m_fpDescPool;
            ai.descriptorSetCount = 1;
            ai.pSetLayouts        = &m_fpDescLayout;
            if (vkAllocateDescriptorSets(m_gpu.device(), &ai, &m_fpSets[f]) != VK_SUCCESS)
                throw std::runtime_error("failed to allocate forward+ descriptor set!");

            VkDescriptorBufferInfo bufs[5]{};
            bufs[0].buffer = m_fpParamsBuffer[f];
            bufs[1].buffer = m_fpLightBuffer[f];
            bufs[2].buffer = m_fpGridBuffer[f];
            bufs[3].buffer = m_fpIndexBuffer[f];
            bufs[4].buffer = m_fpStatsBuffer[f];
            for (int i = 0; i < 5; i++) bufs[i].range = VK_WHOLE_SIZE;

            // La profundidad del depth pre-pass, la misma que muestrea el SSAO, y
            // con su mismo sampler NEAREST: es D32_SFLOAT y el culling la lee a
            // texel exacto.
            VkDescriptorImageInfo depthInfo{};
            depthInfo.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
            depthInfo.imageView   = m_ssaoDepthView[f];
            depthInfo.sampler     = m_ssaoSampler;

            VkWriteDescriptorSet writes[6]{};
            for (int i = 0; i < 6; i++)
            {
                writes[i].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                writes[i].dstSet          = m_fpSets[f];
                writes[i].dstBinding      = (uint32_t)i;
                writes[i].descriptorCount = 1;
                writes[i].descriptorType  = (i == 4) ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
                                                     : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            }
            writes[0].pBufferInfo = &bufs[0];
            writes[1].pBufferInfo = &bufs[1];
            writes[2].pBufferInfo = &bufs[2];
            writes[3].pBufferInfo = &bufs[3];
            writes[4].pImageInfo  = &depthInfo;
            writes[5].pBufferInfo = &bufs[4];

            vkUpdateDescriptorSets(m_gpu.device(), 6, writes, 0, nullptr);
        }
    }

    void Renderer::destroyFpBuffers()
    {
        for (int f = 0; f < MAX_FRAMES; f++)
        {
            if (m_fpGridBuffer[f])
            {
                vkDestroyBuffer(m_gpu.device(), m_fpGridBuffer[f], nullptr);
                m_fpGridBuffer[f] = VK_NULL_HANDLE;
            }
            if (m_fpGridMemory[f])
            {
                vkFreeMemory(m_gpu.device(), m_fpGridMemory[f], nullptr);
                m_fpGridMemory[f] = VK_NULL_HANDLE;
            }
            if (m_fpIndexBuffer[f])
            {
                vkDestroyBuffer(m_gpu.device(), m_fpIndexBuffer[f], nullptr);
                m_fpIndexBuffer[f] = VK_NULL_HANDLE;
            }
            if (m_fpIndexMemory[f])
            {
                vkFreeMemory(m_gpu.device(), m_fpIndexMemory[f], nullptr);
                m_fpIndexMemory[f] = VK_NULL_HANDLE;
            }
            m_fpSets[f] = VK_NULL_HANDLE;
        }
    }

    void Renderer::recordFpCullPass(VkCommandBuffer cmd, const glm::mat4& proj)
    {
        // Apagado: ni un comando. Es lo que hace que la imagen y el coste sean
        // exactamente los de antes de la feature.
        if (m_fpActiveMode == FpMode::Off) { m_fpGpuMs = 0.0f; return; }
        if (m_fpSets[m_currentFrame] == VK_NULL_HANDLE) return;

        // Contadores de hace dos frames en este mismo slot: su fence ya la espero
        // drawFrame, asi que la lectura no bloquea. Se leen ANTES de ponerlos a
        // cero para este frame.
        if (m_fpStatsMapped[m_currentFrame])
        {
            const uint32_t* s = (const uint32_t*)m_fpStatsMapped[m_currentFrame];
            m_fpAvgPerCell    = (s[1] > 0) ? (float)s[0] / (float)s[1] : 0.0f;
            m_fpOverflowCells = s[2];
            memset(m_fpStatsMapped[m_currentFrame], 0, sizeof(uint32_t) * 4);
        }

        if (m_timestampsSupported && m_fpQueryPending[m_currentFrame])
        {
            uint64_t stamps[2] = {};
            if (vkGetQueryPoolResults(m_gpu.device(), m_fpQueryPool, m_currentFrame * 2, 2,
                                      sizeof(stamps), stamps, sizeof(uint64_t),
                                      VK_QUERY_RESULT_64_BIT) == VK_SUCCESS)
            {
                m_fpGpuMs = (float)((double)(stamps[1] - stamps[0]) * m_timestampPeriod * 1e-6);
                if (++m_fpMeasuredFrames == 300)
                {
                    printf("forward+ (%s): culling %.3f ms, %.1f luces/celda, %u celdas desbordadas (%ux%u interno)\n",
                           m_fpActiveMode == FpMode::Tiled ? "tiled" : "clustered",
                           m_fpGpuMs, m_fpAvgPerCell, m_fpOverflowCells,
                           m_renderExtent.width, m_renderExtent.height);
                    fflush(stdout);
                }
            }
        }
        if (m_timestampsSupported)
        {
            vkCmdResetQueryPool(cmd, m_fpQueryPool, m_currentFrame * 2, 2);
            vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, m_fpQueryPool, m_currentFrame * 2);
            m_fpQueryPending[m_currentFrame] = true;
        }

        uint32_t gx = 0, gy = 0, gz = 0, ts = 0;
        fpGridDims(m_fpActiveMode, gx, gy, gz, ts);

        FpPush push{};
        // La proyeccion EFECTIVA del frame, con el Y-flip de Vulkan dentro: es la
        // misma con la que se grabo el depth pre-pass, asi que reconstruir
        // profundidad y levantar los planos del tile es consistente.
        push.p00     = proj[0][0];
        push.p11     = proj[1][1];
        push.p22     = proj[2][2];
        push.p32     = proj[3][2];
        push.screenW = m_renderExtent.width;
        push.screenH = m_renderExtent.height;

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                          m_fpActiveMode == FpMode::Tiled ? m_fpTiledPipeline : m_fpClusteredPipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_fpPipelineLayout,
                                0, 1, &m_fpSets[m_currentFrame], 0, nullptr);
        vkCmdPushConstants(cmd, m_fpPipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);

        if (m_fpActiveMode == FpMode::Tiled)
        {
            // Un workgroup de 16x16 POR TILE: el shader lee un texel por
            // invocacion para reducir el maximo de profundidad del tile.
            vkCmdDispatch(cmd, gx, gy, 1);
        }
        else
        {
            // Una invocacion por cluster, en grupos de 4x4x4.
            vkCmdDispatch(cmd, (gx + 3) / 4, (gy + 3) / 4, (gz + 3) / 4);
        }

        // La rejilla y la lista de indices las lee pbr.frag en el pass de escena.
        VkMemoryBarrier mb{};
        mb.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             0, 1, &mb, 0, nullptr, 0, nullptr);

        // Y los contadores los lee la CPU dos frames despues.
        VkMemoryBarrier hostMb{};
        hostMb.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        hostMb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        hostMb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                             0, 1, &hostMb, 0, nullptr, 0, nullptr);

        if (m_timestampsSupported)
            vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, m_fpQueryPool, m_currentFrame * 2 + 1);
    }

    // ── SSR ─────────────────────────────────────────────────────────────────
    bool Renderer::ssrActive() const
    {
        if (!m_ssrEnabled) return false;
        // Recursos aún sin crear (viewport degenerado): nada que grabar.
        if (m_ssrImage[m_currentFrame] == VK_NULL_HANDLE) return false;
        // Interruptor puesto pero ningún objeto marcado = ningún píxel con
        // máscara: se salta el pass entero en vez de despachar y multiplicar por
        // cero. Es un float por objeto, mucho menos que el culling que ya se hace
        // en este mismo frame.
        for (const RenderObject& o : m_objects)
            if (o.ssrStrength > 0.0f) return true;
        for (const SkinnedRenderObject& o : m_skinnedObjects)
            if (o.ssrStrength > 0.0f) return true;
        return false;
    }

    void Renderer::createSsrPipelines()
    {
        // LINEAR: el impacto del rayo cae entre texeles y R16G16B16A16_SFLOAT sí
        // tiene garantizado el filtrado lineal. La profundidad NO se muestrea con
        // este sampler sino con m_ssaoSampler (NEAREST), que es el que le
        // corresponde a D32_SFLOAT. CLAMP_TO_EDGE para que un tap del borde no
        // traiga color del lado opuesto de la pantalla.
        VkSamplerCreateInfo si{};
        si.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        si.magFilter    = VK_FILTER_LINEAR;
        si.minFilter    = VK_FILTER_LINEAR;
        si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.borderColor  = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
        if (vkCreateSampler(m_gpu.device(), &si, nullptr, &m_ssrSampler) != VK_SUCCESS)
            throw std::runtime_error("failed to create ssr sampler!");

        // Un solo layout para los dos pipelines: color muestreado, profundidad
        // muestreada y destino como storage image. ssr_resolve.comp declara el
        // binding 1 y no lo lee.
        VkDescriptorSetLayoutBinding bindings[3]{};
        for (int i = 0; i < 3; i++)
        {
            bindings[i].binding         = (uint32_t)i;
            bindings[i].descriptorType  = (i < 2) ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
                                                  : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            bindings[i].descriptorCount = 1;
            bindings[i].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
        }

        VkDescriptorSetLayoutCreateInfo dsl{};
        dsl.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        dsl.bindingCount = 3;
        dsl.pBindings    = bindings;
        if (vkCreateDescriptorSetLayout(m_gpu.device(), &dsl, nullptr, &m_ssrDescLayout) != VK_SUCCESS)
            throw std::runtime_error("failed to create ssr descriptor set layout!");

        // Dos sets por frame: marcha (HDR + depth → reflejo) y suma (reflejo →
        // HDR).
        const uint32_t ssrSets = MAX_FRAMES * 2;
        VkDescriptorPoolSize sizes[2]{};
        sizes[0].type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        sizes[0].descriptorCount = ssrSets * 2;
        sizes[1].type            = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        sizes[1].descriptorCount = ssrSets;

        VkDescriptorPoolCreateInfo dpi{};
        dpi.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        dpi.poolSizeCount = 2;
        dpi.pPoolSizes    = sizes;
        dpi.maxSets       = ssrSets;
        if (vkCreateDescriptorPool(m_gpu.device(), &dpi, nullptr, &m_ssrDescPool) != VK_SUCCESS)
            throw std::runtime_error("failed to create ssr descriptor pool!");

        VkPushConstantRange pcr{};
        pcr.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        pcr.offset     = 0;
        pcr.size       = sizeof(SsrPush);

        VkPipelineLayoutCreateInfo pli{};
        pli.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pli.setLayoutCount         = 1;
        pli.pSetLayouts            = &m_ssrDescLayout;
        pli.pushConstantRangeCount = 1;
        pli.pPushConstantRanges    = &pcr;
        if (vkCreatePipelineLayout(m_gpu.device(), &pli, nullptr, &m_ssrPipelineLayout) != VK_SUCCESS)
            throw std::runtime_error("failed to create ssr pipeline layout!");

        auto makeSsrPipeline = [&](const std::string& spv, VkPipeline& pipeline)
        {
            auto code   = loadShaderFile(spv);
            auto module = createShaderModule(code);

            VkComputePipelineCreateInfo ci{};
            ci.sType        = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
            ci.stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            ci.stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
            ci.stage.module = module;
            ci.stage.pName  = "main";
            ci.layout       = m_ssrPipelineLayout;
            if (vkCreateComputePipelines(m_gpu.device(), VK_NULL_HANDLE, 1, &ci, nullptr, &pipeline) != VK_SUCCESS)
                throw std::runtime_error("failed to create compute pipeline: " + spv);

            vkDestroyShaderModule(m_gpu.device(), module, nullptr);
        };

        makeSsrPipeline("shaders/ssr.comp.spv",         m_ssrPipeline);
        makeSsrPipeline("shaders/ssr_resolve.comp.spv", m_ssrResolvePipeline);

        // Cuatro por frame: [0,1] el depth pre-pass cuando lo pide el SSR, [2,3]
        // los dos dispatches. m_timestampsSupported ya lo resolvió el bloom.
        if (m_timestampsSupported)
        {
            VkQueryPoolCreateInfo qpi{};
            qpi.sType      = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
            qpi.queryType  = VK_QUERY_TYPE_TIMESTAMP;
            qpi.queryCount = MAX_FRAMES * 4;
            if (vkCreateQueryPool(m_gpu.device(), &qpi, nullptr, &m_ssrQueryPool) != VK_SUCCESS)
                throw std::runtime_error("failed to create ssr query pool!");
        }

        printf("ssr pipelines OK\n"); fflush(stdout);
    }

    void Renderer::createSsrImages()
    {
        for (int f = 0; f < MAX_FRAMES; f++)
        {
            // Mismo formato que el HDR: ssr_resolve.comp declara los dos con el
            // qualifier rgba16f. Resolución completa, como el SSAO.
            m_res.createImage(
                m_renderExtent.width, m_renderExtent.height,
                kHdrFormat, VK_IMAGE_TILING_OPTIMAL,
                VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                m_ssrImage[f], m_ssrMemory[f]);
            m_res.createTextureImageView(m_ssrImage[f], m_ssrView[f], kHdrFormat);
        }

        // Los sets de la vez anterior apuntan a vistas ya destruidas: reset y no
        // free, igual que en el bloom y en el SSAO.
        vkResetDescriptorPool(m_gpu.device(), m_ssrDescPool, 0);

        for (int f = 0; f < MAX_FRAMES; f++)
        {
            VkDescriptorSetLayout layouts[2] = { m_ssrDescLayout, m_ssrDescLayout };
            VkDescriptorSet       sets[2]    = {};

            VkDescriptorSetAllocateInfo ai{};
            ai.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            ai.descriptorPool     = m_ssrDescPool;
            ai.descriptorSetCount = 2;
            ai.pSetLayouts        = layouts;
            if (vkAllocateDescriptorSets(m_gpu.device(), &ai, sets) != VK_SUCCESS)
                throw std::runtime_error("failed to allocate ssr descriptor sets!");

            m_ssrSets[f]        = sets[0];
            m_ssrResolveSets[f] = sets[1];

            VkDescriptorImageInfo infos[6]{};
            // Marcha: color de la escena (sale del render pass en SHADER_READ_ONLY)
            // + profundidad del pre-pass → reflejo.
            infos[0].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            infos[0].imageView   = m_hdrView[f];
            infos[0].sampler     = m_ssrSampler;
            infos[1].imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
            infos[1].imageView   = m_ssaoDepthView[f];
            infos[1].sampler     = m_ssaoSampler;
            infos[2].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
            infos[2].imageView   = m_ssrView[f];
            // Suma: el reflejo (ya en GENERAL) → el HDR como storage. El binding 1
            // se rellena con la misma profundidad aunque el shader no lo lea: un
            // descriptor set no puede quedarse con un binding sin escribir.
            infos[3].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
            infos[3].imageView   = m_ssrView[f];
            infos[3].sampler     = m_ssrSampler;
            infos[4]             = infos[1];
            infos[5].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
            infos[5].imageView   = m_hdrView[f];

            VkWriteDescriptorSet writes[6]{};
            for (int i = 0; i < 6; i++)
            {
                writes[i].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                writes[i].dstSet          = (i < 3) ? m_ssrSets[f] : m_ssrResolveSets[f];
                writes[i].dstBinding      = (uint32_t)(i % 3);
                writes[i].descriptorType  = (i % 3 == 2) ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
                                                         : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                writes[i].descriptorCount = 1;
                writes[i].pImageInfo      = &infos[i];
            }
            vkUpdateDescriptorSets(m_gpu.device(), 6, writes, 0, nullptr);
        }
    }

    void Renderer::destroySsrImages()
    {
        for (int f = 0; f < MAX_FRAMES; f++)
        {
            if (m_ssrView[f])
            {
                vkDestroyImageView(m_gpu.device(), m_ssrView[f], nullptr);
                m_ssrView[f] = VK_NULL_HANDLE;
            }
            if (m_ssrImage[f])
            {
                vkDestroyImage(m_gpu.device(), m_ssrImage[f], nullptr);
                m_ssrImage[f] = VK_NULL_HANDLE;
            }
            if (m_ssrMemory[f])
            {
                vkFreeMemory(m_gpu.device(), m_ssrMemory[f], nullptr);
                m_ssrMemory[f] = VK_NULL_HANDLE;
            }
            m_ssrSets[f]        = VK_NULL_HANDLE;
            m_ssrResolveSets[f] = VK_NULL_HANDLE;
        }
    }

    void Renderer::recordSsrPass(VkCommandBuffer cmd, const glm::mat4& proj)
    {
        if (!ssrActive())
        {
            m_ssrGpuMs = 0.0f;
            // Ni dispatches ni barreras: el HDR se queda tal y como lo dejó el
            // pass de escena, en SHADER_READ_ONLY, que es justo lo que esperan el
            // bloom y la composición. Imagen idéntica a la de antes del SSR.
            return;
        }

        // Timestamps de hace dos frames en este mismo slot, ya señalados.
        if (m_timestampsSupported && m_ssrQueryPending[m_currentFrame])
        {
            uint64_t stamps[4] = {};
            if (vkGetQueryPoolResults(m_gpu.device(), m_ssrQueryPool, m_currentFrame * 4, 4,
                                      sizeof(stamps), stamps, sizeof(uint64_t),
                                      VK_QUERY_RESULT_64_BIT) == VK_SUCCESS)
            {
                // El pre-pass solo cuenta como coste del SSR cuando es el SSR
                // quien lo pide: con el SSAO encendido ya sale en ssaoGpuMs y
                // sumarlo aquí lo contaría dos veces.
                const uint64_t prepass = ssaoEnabled() ? 0 : (stamps[1] - stamps[0]);
                m_ssrGpuMs = (float)((double)(prepass + (stamps[3] - stamps[2]))
                                     * m_timestampPeriod * 1e-6);
                if (++m_ssrMeasuredFrames == 300)
                {
                    printf("ssr (marcha + suma%s): %.3f ms (%ux%u, %d pasos)\n",
                           ssaoEnabled() ? "" : " + depth pre-pass",
                           m_ssrGpuMs, m_swapChainExtent.width, m_swapChainExtent.height,
                           m_ssrMaxSteps);
                    fflush(stdout);
                }
            }
        }
        // Solo se da por bueno el frame en el que recordSsaoPass dejó escrito el
        // par [0,1]: sin eso la lectura de los cuatro daría NOT_READY.
        if (m_timestampsSupported && m_ssrStampedPrepass)
        {
            vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, m_ssrQueryPool, m_currentFrame * 4 + 2);
            m_ssrQueryPending[m_currentFrame] = true;
        }
        else
        {
            m_ssrQueryPending[m_currentFrame] = false;
        }

        SsrPush push{};
        // Los mismos cuatro coeficientes que usa el SSAO, de la proyección
        // EFECTIVA del frame (Y-flip incluido): es la que grabó el depth, así que
        // reconstruir y reproyectar es consistente.
        push.projP00     = proj[0][0];
        push.projP11     = proj[1][1];
        push.projP22     = proj[2][2];
        push.projP32     = proj[3][2];
        push.invResX     = 1.0f / (float)m_renderExtent.width;
        push.invResY     = 1.0f / (float)m_renderExtent.height;
        push.maxDistance = m_ssrMaxDistance;
        push.thickness   = m_ssrThickness;
        push.maxSteps    = (int32_t)m_ssrMaxSteps;
        // Fijo y no configurable: cuatro bisecciones ya sitúan el impacto dentro
        // de 1/16 de paso, y subirlo no cambia nada visible.
        push.refineSteps = 4;
        push.edgeFade    = m_ssrEdgeFade;
        push.intensity   = m_ssrIntensity;

        VkImageMemoryBarrier b{};
        b.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        b.subresourceRange.baseMipLevel   = 0;
        b.subresourceRange.levelCount     = 1;
        b.subresourceRange.baseArrayLayer = 0;
        b.subresourceRange.layerCount     = 1;

        // El reflejo entra desde UNDEFINED: se reescribe entero (ssr.comp empieza
        // por poner el píxel a 0) y el contenido del frame anterior no se
        // reutiliza.
        b.image         = m_ssrImage[m_currentFrame];
        b.oldLayout     = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout     = VK_IMAGE_LAYOUT_GENERAL;
        b.srcAccessMask = 0;
        b.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &b);

        const uint32_t gx = (m_renderExtent.width  + 7) / 8;
        const uint32_t gy = (m_renderExtent.height + 7) / 8;

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_ssrPipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_ssrPipelineLayout,
                                0, 1, &m_ssrSets[m_currentFrame], 0, nullptr);
        vkCmdPushConstants(cmd, m_ssrPipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
        vkCmdDispatch(cmd, gx, gy, 1);

        // Dos transiciones antes de la suma: el reflejo que se acaba de escribir
        // pasa a leerse, y el HDR sale de SHADER_READ_ONLY (donde lo dejó el
        // render pass, y desde donde acaba de leerlo la marcha) a GENERAL, que es
        // el único layout válido para imageLoad/imageStore.
        VkImageMemoryBarrier toResolve[2] = { b, b };
        toResolve[0].image         = m_ssrImage[m_currentFrame];
        toResolve[0].oldLayout     = VK_IMAGE_LAYOUT_GENERAL;
        toResolve[0].newLayout     = VK_IMAGE_LAYOUT_GENERAL;
        toResolve[0].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        toResolve[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        toResolve[1].image         = m_hdrImage[m_currentFrame];
        toResolve[1].oldLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        toResolve[1].newLayout     = VK_IMAGE_LAYOUT_GENERAL;
        toResolve[1].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        toResolve[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             0, 0, nullptr, 0, nullptr, 2, toResolve);

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_ssrResolvePipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_ssrPipelineLayout,
                                0, 1, &m_ssrResolveSets[m_currentFrame], 0, nullptr);
        vkCmdPushConstants(cmd, m_ssrPipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
        vkCmdDispatch(cmd, gx, gy, 1);

        // Y el HDR vuelve a SHADER_READ_ONLY, que es el layout que declaran los
        // descriptor sets del bloom (compute) y de la composición (fragment).
        b.image         = m_hdrImage[m_currentFrame];
        b.oldLayout     = VK_IMAGE_LAYOUT_GENERAL;
        b.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        b.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &b);

        if (m_timestampsSupported && m_ssrQueryPending[m_currentFrame])
            vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, m_ssrQueryPool, m_currentFrame * 4 + 3);
    }

    // ── Niebla volumetrica ──────────────────────────────────────────────────
    FogPass::Context Renderer::fogCtx()
    {
        return FogPass::Context{
            m_gpu, *this, m_renderExtent, m_currentFrame,
            m_hdrImage, m_hdrView, m_ssaoDepthView, m_ssaoSampler,
            m_uniformBuffers, m_shadowView, m_shadowSampler, m_lights,
            m_timestampsSupported, m_timestampPeriod
        };
    }

    // ── Motion blur ─────────────────────────────────────────────────────────
    MotionBlurPass::Context Renderer::motionBlurCtx()
    {
        return MotionBlurPass::Context{
            m_gpu, m_res, *this, m_renderExtent, m_currentFrame, kHdrFormat,
            m_hdrImage, m_hdrView, m_ssaoDepthView,
            m_ssrSampler, m_ssaoSampler,
            m_taaCurrViewProj, m_taaPrevViewProj
        };
    }

    void Renderer::createAaRenderPasses()
    {
        // Un solo attachment: m_offscreenImage, la de siempre. El triangulo la
        // cubre entera, asi que no hay nada que cargar. Sin depth: el contorno y
        // los gizmos ya se dibujaron en el pass de composicion, aguas arriba.
        VkAttachmentDescription colorAtt{};
        colorAtt.format         = m_swapChainFormat;
        colorAtt.samples        = VK_SAMPLE_COUNT_1_BIT;
        colorAtt.loadOp         = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        colorAtt.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
        colorAtt.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        colorAtt.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        colorAtt.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
        // El MISMO finalLayout que deja el pass de composicion cuando el FXAA
        // esta apagado: encender o apagar el efecto en caliente no deja a
        // m_offscreenImage en un layout distinto del que espera la UI o el blit.
        colorAtt.finalLayout    = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkAttachmentReference colorRef{};
        colorRef.attachment = 0;
        colorRef.layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments    = &colorRef;

        VkSubpassDependency deps[2]{};
        // Entrada: espera a que el pass de composicion haya terminado de escribir
        // la imagen intermedia, que es lo unico que muestrea este pass.
        deps[0].srcSubpass    = VK_SUBPASS_EXTERNAL;
        deps[0].dstSubpass    = 0;
        deps[0].srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        deps[0].dstStageMask  = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
                              | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        deps[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        deps[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

        // Salida: la UI (o el blit headless) lee la imagen ya suavizada.
        deps[1].srcSubpass      = 0;
        deps[1].dstSubpass      = VK_SUBPASS_EXTERNAL;
        deps[1].srcStageMask    = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        deps[1].dstStageMask    = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        deps[1].srcAccessMask   = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        deps[1].dstAccessMask   = VK_ACCESS_SHADER_READ_BIT;
        deps[1].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

        VkRenderPassCreateInfo rpInfo{};
        rpInfo.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        rpInfo.attachmentCount = 1;
        rpInfo.pAttachments    = &colorAtt;
        rpInfo.subpassCount    = 1;
        rpInfo.pSubpasses      = &subpass;
        rpInfo.dependencyCount = 2;
        rpInfo.pDependencies   = deps;

        if (vkCreateRenderPass(m_gpu.device(), &rpInfo, nullptr, &m_aaRenderPass) != VK_SUCCESS)
            throw std::runtime_error("failed to create aa render pass!");

        // --- Variante del TAA: dos attachments ------------------------------
        // El mismo color va a la vez a m_offscreenImage (que se presenta) y al
        // historial (que se muestrea el frame siguiente). Escribirlo una vez con
        // dos targets ahorra un segundo pass entero sobre toda la pantalla.
        VkAttachmentDescription taaAtts[2] = { colorAtt, colorAtt };
        // El historial no se presenta: sale listo para que lo lea taa.frag.
        taaAtts[1].finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkAttachmentReference taaRefs[2]{};
        taaRefs[0].attachment = 0;
        taaRefs[0].layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        taaRefs[1].attachment = 1;
        taaRefs[1].layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        VkSubpassDescription taaSubpass{};
        taaSubpass.pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS;
        taaSubpass.colorAttachmentCount = 2;
        taaSubpass.pColorAttachments    = taaRefs;

        VkRenderPassCreateInfo taaRpInfo = rpInfo;
        taaRpInfo.attachmentCount = 2;
        taaRpInfo.pAttachments    = taaAtts;
        taaRpInfo.pSubpasses      = &taaSubpass;

        if (vkCreateRenderPass(m_gpu.device(), &taaRpInfo, nullptr, &m_taaHistoryRenderPass) != VK_SUCCESS)
            throw std::runtime_error("failed to create taa render pass!");

        printf("aa render passes OK\n"); fflush(stdout);
    }

    void Renderer::createAaPipelines()
    {
        // Filtrado LINEAL: los tres modos muestrean entre texeles (FXAA a media
        // distancia, SSAA en la rejilla de bajada, TAA en la uv reproyectada) y
        // es de ahi de donde sale el suavizado. Con NEAREST no harian nada.
        VkSamplerCreateInfo si{};
        si.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        si.magFilter    = VK_FILTER_LINEAR;
        si.minFilter    = VK_FILTER_LINEAR;
        si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.borderColor  = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
        if (vkCreateSampler(m_gpu.device(), &si, nullptr, &m_aaSampler) != VK_SUCCESS)
            throw std::runtime_error("failed to create aa sampler!");

        // --- Layout de un binding: la imagen intermedia. FXAA y SSAA -------
        VkDescriptorSetLayoutBinding binding{};
        binding.binding         = 0;
        binding.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        binding.descriptorCount = 1;
        binding.stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;

        VkDescriptorSetLayoutCreateInfo dsl{};
        dsl.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        dsl.bindingCount = 1;
        dsl.pBindings    = &binding;
        if (vkCreateDescriptorSetLayout(m_gpu.device(), &dsl, nullptr, &m_aaDescLayout) != VK_SUCCESS)
            throw std::runtime_error("failed to create aa descriptor set layout!");

        VkDescriptorPoolSize poolSize{};
        poolSize.type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        poolSize.descriptorCount = MAX_FRAMES;

        VkDescriptorPoolCreateInfo dpi{};
        dpi.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        dpi.poolSizeCount = 1;
        dpi.pPoolSizes    = &poolSize;
        dpi.maxSets       = MAX_FRAMES;
        if (vkCreateDescriptorPool(m_gpu.device(), &dpi, nullptr, &m_aaDescPool) != VK_SUCCESS)
            throw std::runtime_error("failed to create aa descriptor pool!");

        // --- Layout de tres bindings: color, historial y profundidad. TAA ---
        VkDescriptorSetLayoutBinding taaBindings[3]{};
        for (int i = 0; i < 3; i++)
        {
            taaBindings[i].binding         = (uint32_t)i;
            taaBindings[i].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            taaBindings[i].descriptorCount = 1;
            taaBindings[i].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
        }
        VkDescriptorSetLayoutCreateInfo taaDsl{};
        taaDsl.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        taaDsl.bindingCount = 3;
        taaDsl.pBindings    = taaBindings;
        if (vkCreateDescriptorSetLayout(m_gpu.device(), &taaDsl, nullptr, &m_taaDescLayout) != VK_SUCCESS)
            throw std::runtime_error("failed to create taa descriptor set layout!");

        VkDescriptorPoolSize taaPoolSize{};
        taaPoolSize.type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        taaPoolSize.descriptorCount = MAX_FRAMES * 3;

        VkDescriptorPoolCreateInfo taaDpi{};
        taaDpi.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        taaDpi.poolSizeCount = 1;
        taaDpi.pPoolSizes    = &taaPoolSize;
        taaDpi.maxSets       = MAX_FRAMES;
        if (vkCreateDescriptorPool(m_gpu.device(), &taaDpi, nullptr, &m_taaDescPool) != VK_SUCCESS)
            throw std::runtime_error("failed to create taa descriptor pool!");

        // Un pipeline layout por modo: las push constants no tienen el mismo
        // tamano y el TAA ademas usa otro descriptor set layout.
        auto makeLayout = [&](VkDescriptorSetLayout setLayout, uint32_t pushSize, VkPipelineLayout& out)
        {
            VkPushConstantRange pcr{};
            pcr.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
            pcr.offset     = 0;
            pcr.size       = pushSize;

            VkPipelineLayoutCreateInfo pli{};
            pli.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
            pli.setLayoutCount         = 1;
            pli.pSetLayouts            = &setLayout;
            pli.pushConstantRangeCount = 1;
            pli.pPushConstantRanges    = &pcr;
            if (vkCreatePipelineLayout(m_gpu.device(), &pli, nullptr, &out) != VK_SUCCESS)
                throw std::runtime_error("failed to create aa pipeline layout!");
        };
        makeLayout(m_aaDescLayout,  (uint32_t)sizeof(FxaaPush), m_fxaaPipelineLayout);
        makeLayout(m_aaDescLayout,  (uint32_t)sizeof(SsaaPush), m_ssaaPipelineLayout);
        makeLayout(m_taaDescLayout, (uint32_t)sizeof(TaaPush),  m_taaPipelineLayout);

        // Mismo vertex shader que la composicion: el triangulo sale de
        // gl_VertexIndex y saca la UV en location 0, que es justo lo que esperan
        // los tres fragment shaders.
        auto vertCode = loadShaderFile("shaders/fullscreen.vert.spv");
        VkShaderModule vertModule = createShaderModule(vertCode);

        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vertModule;
        stages[0].pName  = "main";
        stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].pName  = "main";

        VkPipelineVertexInputStateCreateInfo vi{};
        vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

        VkPipelineInputAssemblyStateCreateInfo ia{};
        ia.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

        VkPipelineViewportStateCreateInfo vp{};
        vp.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        vp.viewportCount = 1;
        vp.scissorCount  = 1;

        VkPipelineRasterizationStateCreateInfo rs{};
        rs.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.cullMode    = VK_CULL_MODE_NONE;
        rs.lineWidth   = 1.0f;

        VkPipelineMultisampleStateCreateInfo ms{};
        ms.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        // El TAA escribe en DOS attachments (pantalla + historial) y el estado de
        // blending tiene que declarar uno por attachment o el pipeline es
        // invalido, aunque los dos sean identicos.
        VkPipelineColorBlendAttachmentState blend[2]{};
        for (int i = 0; i < 2; i++)
            blend[i].colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                      VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

        VkPipelineColorBlendStateCreateInfo cb{};
        cb.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        cb.attachmentCount = 1;
        cb.pAttachments    = blend;

        VkDynamicState dynStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo dyn{};
        dyn.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dyn.dynamicStateCount = 2;
        dyn.pDynamicStates    = dynStates;

        VkGraphicsPipelineCreateInfo pci{};
        pci.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        pci.stageCount          = 2;
        pci.pStages             = stages;
        pci.pVertexInputState   = &vi;
        pci.pInputAssemblyState = &ia;
        pci.pViewportState      = &vp;
        pci.pRasterizationState = &rs;
        pci.pMultisampleState   = &ms;
        // Sin pDepthStencilState: ningun subpass de resolucion declara attachment
        // de profundidad, asi que Vulkan permite (y espera) un puntero nulo.
        pci.pDepthStencilState  = nullptr;
        pci.pColorBlendState    = &cb;
        pci.pDynamicState       = &dyn;
        pci.subpass             = 0;

        // Los tres pipelines comparten TODO el estado fijo: solo cambian el
        // fragment shader, el pipeline layout y (en el TAA) el render pass y el
        // numero de attachments.
        auto makePipeline = [&](const char* spv, VkPipelineLayout layout,
                                VkRenderPass pass, uint32_t attachments, VkPipeline& out)
        {
            auto code = loadShaderFile(spv);
            VkShaderModule module = createShaderModule(code);
            stages[1].module   = module;
            cb.attachmentCount = attachments;
            pci.layout         = layout;
            pci.renderPass     = pass;
            if (vkCreateGraphicsPipelines(m_gpu.device(), VK_NULL_HANDLE, 1, &pci, nullptr, &out) != VK_SUCCESS)
                throw std::runtime_error(std::string("failed to create graphics pipeline: ") + spv);
            vkDestroyShaderModule(m_gpu.device(), module, nullptr);
        };

        makePipeline("shaders/fxaa.frag.spv",         m_fxaaPipelineLayout, m_aaRenderPass,         1, m_fxaaPipeline);
        makePipeline("shaders/ssaa_resolve.frag.spv", m_ssaaPipelineLayout, m_aaRenderPass,         1, m_ssaaPipeline);
        makePipeline("shaders/taa.frag.spv",          m_taaPipelineLayout,  m_taaHistoryRenderPass, 2, m_taaPipeline);

        vkDestroyShaderModule(m_gpu.device(), vertModule, nullptr);

        // m_timestampsSupported y m_timestampPeriod los resolvio
        // createBloomPipelines, que corre antes. Cuatro queries por frame: [0,1]
        // el pass propio del modo, [2,3] el render completo sin UI.
        if (m_timestampsSupported)
        {
            VkQueryPoolCreateInfo qpi{};
            qpi.sType      = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
            qpi.queryType  = VK_QUERY_TYPE_TIMESTAMP;
            qpi.queryCount = MAX_FRAMES * 4;
            if (vkCreateQueryPool(m_gpu.device(), &qpi, nullptr, &m_aaQueryPool) != VK_SUCCESS)
                throw std::runtime_error("failed to create aa query pool!");

            // Pool del panel Performance: [0,1] sombras, [2,3] escena. Se crea
            // aqui (y no en su propia funcion) porque este es el ultimo sitio
            // del arranque donde m_timestampsSupported ya esta resuelto y el
            // device sigue vivo. Con el panel cerrado no se usa ni una query.
            qpi.queryCount = MAX_FRAMES * 4;
            if (vkCreateQueryPool(m_gpu.device(), &qpi, nullptr, &m_perfQueryPool) != VK_SUCCESS)
                throw std::runtime_error("failed to create perf query pool!");
        }

        printf("aa pipelines OK\n"); fflush(stdout);
    }

    void Renderer::createAaImages()
    {
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

        const bool msaa = (m_aaActiveMode == AaMode::Msaa);
        const bool taa  = (m_aaActiveMode == AaMode::Taa);

        for (int f = 0; f < MAX_FRAMES; f++)
        {
            if (msaa)
            {
                // Color multisample de la escena y de la composicion. Los dos se
                // resuelven dentro de su render pass sobre las imagenes de una
                // muestra de siempre, asi que nada de lo que hay detras (SSAO,
                // SSR, bloom, UI, blit) se entera de que existen.
                createMsImage(kHdrFormat,       m_msaaHdrImage[f], m_msaaHdrMemory[f], m_msaaHdrView[f]);
                createMsImage(m_swapChainFormat, m_msaaLdrImage[f], m_msaaLdrMemory[f], m_msaaLdrView[f]);
            }

            if (!needsAaIntermediate()) continue;

            // Destino alternativo de la composicion. Tiene el tamano INTERNO del
            // render, que en SSAA no es el de la ventana. COLOR_ATTACHMENT porque
            // es un target de render, SAMPLED porque el pass de resolucion lo lee.
            m_res.createImage(
                m_renderExtent.width, m_renderExtent.height,
                m_swapChainFormat, VK_IMAGE_TILING_OPTIMAL,
                VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                m_aaSrcImage[f], m_aaSrcMemory[f]);
            m_res.createTextureImageView(m_aaSrcImage[f], m_aaSrcView[f], m_swapChainFormat);

            // Framebuffer del pass de COMPOSICION apuntando aqui, con el mismo
            // depth compartido que el framebuffer de siempre: el contorno y los
            // gizmos siguen cargando la profundidad de la escena.
            VkImageView compAtts[] = { m_aaSrcView[f], m_depthImageView };
            VkFramebufferCreateInfo fbInfo{};
            fbInfo.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            fbInfo.renderPass      = m_compositeRenderPass;
            fbInfo.attachmentCount = 2;
            fbInfo.pAttachments    = compAtts;
            fbInfo.width           = m_renderExtent.width;
            fbInfo.height          = m_renderExtent.height;
            fbInfo.layers          = 1;
            if (vkCreateFramebuffer(m_gpu.device(), &fbInfo, nullptr, &m_aaSrcFramebuffer[f]) != VK_SUCCESS)
                throw std::runtime_error("failed to create aa source framebuffer!");

            if (taa)
            {
                // Historial: mismo formato y tamano que la imagen que se
                // presenta. TRANSFER_DST no se usa para copiar nada: es el
                // requisito de la transicion inicial de layout de aqui abajo.
                m_res.createImage(
                    effectiveViewport().width, effectiveViewport().height,
                    m_swapChainFormat, VK_IMAGE_TILING_OPTIMAL,
                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                        VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                    m_taaHistoryImage[f], m_taaHistoryMemory[f]);
                m_res.createTextureImageView(m_taaHistoryImage[f], m_taaHistoryView[f], m_swapChainFormat);

                // El historial se MUESTREA antes de escribirse: el primer frame
                // de cada slot (y el primero tras cada resize) lo lee todavia
                // recien creado. taa.frag descarta ese contenido por
                // historyValid, pero el descriptor lo declara en
                // SHADER_READ_ONLY y la capa de validacion exige que la imagen
                // este de verdad en ese layout, no en UNDEFINED. Se pasa por
                // TRANSFER_DST porque es la unica cadena que admite
                // transitionImageLayout; no se copia nada.
                m_res.transitionImageLayout(m_taaHistoryImage[f],
                    VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
                m_res.transitionImageLayout(m_taaHistoryImage[f],
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

                // Un solo pass escribe los dos: la imagen que se presenta y el
                // historial que se leera el frame siguiente.
                VkImageView taaAtts[] = { m_offscreenView[f], m_taaHistoryView[f] };
                VkFramebufferCreateInfo taaFb = fbInfo;
                taaFb.renderPass      = m_taaHistoryRenderPass;
                taaFb.attachmentCount = 2;
                taaFb.pAttachments    = taaAtts;
                taaFb.width           = effectiveViewport().width;
                taaFb.height          = effectiveViewport().height;
                if (vkCreateFramebuffer(m_gpu.device(), &taaFb, nullptr, &m_taaHistoryFramebuffer[f]) != VK_SUCCESS)
                    throw std::runtime_error("failed to create taa framebuffer!");
            }
            else
            {
                // Framebuffer del pass de resolucion: escribe en la offscreen de
                // siempre, que es la que muestrea la UI y la que blitea el
                // runtime headless. Va a tamano de VENTANA aunque la fuente sea
                // mayor: eso es exactamente el downsample del SSAA.
                VkFramebufferCreateInfo outFb = fbInfo;
                outFb.renderPass      = m_aaRenderPass;
                outFb.attachmentCount = 1;
                outFb.pAttachments    = &m_offscreenView[f];
                outFb.width           = effectiveViewport().width;
                outFb.height          = effectiveViewport().height;
                if (vkCreateFramebuffer(m_gpu.device(), &outFb, nullptr, &m_aaFramebuffer[f]) != VK_SUCCESS)
                    throw std::runtime_error("failed to create aa framebuffer!");
            }
        }

        if (!needsAaIntermediate()) return;

        // Los sets de la vez anterior apuntan a vistas ya destruidas: reset y no
        // free, igual que en el bloom, el SSAO y el SSR.
        vkResetDescriptorPool(m_gpu.device(), m_aaDescPool, 0);
        if (taa) vkResetDescriptorPool(m_gpu.device(), m_taaDescPool, 0);

        for (int f = 0; f < MAX_FRAMES; f++)
        {
            if (taa)
            {
                VkDescriptorSetAllocateInfo ai{};
                ai.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
                ai.descriptorPool     = m_taaDescPool;
                ai.descriptorSetCount = 1;
                ai.pSetLayouts        = &m_taaDescLayout;
                if (vkAllocateDescriptorSets(m_gpu.device(), &ai, &m_taaSets[f]) != VK_SUCCESS)
                    throw std::runtime_error("failed to allocate taa descriptor set!");

                VkDescriptorImageInfo infos[3]{};
                infos[0].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                infos[0].imageView   = m_aaSrcView[f];
                infos[0].sampler     = m_aaSampler;
                // El historial que se LEE es el del otro slot: el que escribio el
                // frame anterior. Con MAX_FRAMES = 2 alternan solos.
                infos[1].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                infos[1].imageView   = m_taaHistoryView[(f + 1) % MAX_FRAMES];
                infos[1].sampler     = m_aaSampler;
                // Profundidad del depth pre-pass, que ya sale en el layout de
                // lectura y se graba sin jitter (es la geometrica).
                infos[2].imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
                infos[2].imageView   = m_ssaoDepthView[f];
                infos[2].sampler     = m_ssaoSampler;

                VkWriteDescriptorSet writes[3]{};
                for (int i = 0; i < 3; i++)
                {
                    writes[i].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                    writes[i].dstSet          = m_taaSets[f];
                    writes[i].dstBinding      = (uint32_t)i;
                    writes[i].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                    writes[i].descriptorCount = 1;
                    writes[i].pImageInfo      = &infos[i];
                }
                vkUpdateDescriptorSets(m_gpu.device(), 3, writes, 0, nullptr);
                continue;
            }

            VkDescriptorSetAllocateInfo ai{};
            ai.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            ai.descriptorPool     = m_aaDescPool;
            ai.descriptorSetCount = 1;
            ai.pSetLayouts        = &m_aaDescLayout;
            if (vkAllocateDescriptorSets(m_gpu.device(), &ai, &m_aaSets[f]) != VK_SUCCESS)
                throw std::runtime_error("failed to allocate aa descriptor set!");

            VkDescriptorImageInfo info{};
            info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            info.imageView   = m_aaSrcView[f];
            info.sampler     = m_aaSampler;

            VkWriteDescriptorSet write{};
            write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write.dstSet          = m_aaSets[f];
            write.dstBinding      = 0;
            write.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            write.descriptorCount = 1;
            write.pImageInfo      = &info;
            vkUpdateDescriptorSets(m_gpu.device(), 1, &write, 0, nullptr);
        }
    }

    void Renderer::destroyAaImages()
    {
        auto destroyImage = [&](VkImage& image, VkDeviceMemory& memory, VkImageView& view)
        {
            if (view)   { vkDestroyImageView(m_gpu.device(), view, nullptr);  view   = VK_NULL_HANDLE; }
            if (image)  { vkDestroyImage(m_gpu.device(), image, nullptr);     image  = VK_NULL_HANDLE; }
            if (memory) { vkFreeMemory(m_gpu.device(), memory, nullptr);      memory = VK_NULL_HANDLE; }
        };

        for (int f = 0; f < MAX_FRAMES; f++)
        {
            if (m_aaFramebuffer[f])
            {
                vkDestroyFramebuffer(m_gpu.device(), m_aaFramebuffer[f], nullptr);
                m_aaFramebuffer[f] = VK_NULL_HANDLE;
            }
            if (m_aaSrcFramebuffer[f])
            {
                vkDestroyFramebuffer(m_gpu.device(), m_aaSrcFramebuffer[f], nullptr);
                m_aaSrcFramebuffer[f] = VK_NULL_HANDLE;
            }
            if (m_taaHistoryFramebuffer[f])
            {
                vkDestroyFramebuffer(m_gpu.device(), m_taaHistoryFramebuffer[f], nullptr);
                m_taaHistoryFramebuffer[f] = VK_NULL_HANDLE;
            }
            destroyImage(m_aaSrcImage[f],      m_aaSrcMemory[f],      m_aaSrcView[f]);
            destroyImage(m_taaHistoryImage[f], m_taaHistoryMemory[f], m_taaHistoryView[f]);
            destroyImage(m_msaaHdrImage[f],    m_msaaHdrMemory[f],    m_msaaHdrView[f]);
            destroyImage(m_msaaLdrImage[f],    m_msaaLdrMemory[f],    m_msaaLdrView[f]);
            m_aaSets[f]  = VK_NULL_HANDLE;
            m_taaSets[f] = VK_NULL_HANDLE;
        }
        // El historial que quede es de un tamano o un modo que ya no existe.
        m_taaHistoryValid = false;
    }

    void Renderer::recordAaPass(VkCommandBuffer cmd)
    {
        if (!needsAaIntermediate())
        {
            // None y MSAA no tienen pass propio. En MSAA el resolve ocurre dentro
            // del pass de composicion y su coste sale en renderGpuMs(); en None no
            // hay ni un comando de mas: la composicion ya escribio directamente en
            // m_offscreenImage y la dejo en SHADER_READ_ONLY, que es exactamente
            // lo que esperan la UI y el blit headless.
            m_aaGpuMs = 0.0f;
            m_aaPassStamped[m_currentFrame] = false;
            return;
        }

        const bool taa = (m_aaActiveMode == AaMode::Taa);
        const VkFramebuffer fb = taa ? m_taaHistoryFramebuffer[m_currentFrame]
                                     : m_aaFramebuffer[m_currentFrame];
        // Red de seguridad: el modo activo y los recursos construidos van
        // siempre a la par (m_aaActiveMode solo cambia dentro de
        // rebuildAaResources), pero grabar un render pass con un framebuffer
        // nulo mata el proceso. Si algun dia se vuelven a desincronizar, se
        // pierde el anti-aliasing de un frame en vez de la aplicacion entera.
        if (fb == VK_NULL_HANDLE)
        {
            m_aaGpuMs = 0.0f;
            m_aaPassStamped[m_currentFrame] = false;
            return;
        }

        if (m_timestampsSupported)
        {
            // El pool ya lo reseteo el arranque del frame: aqui solo se escribe.
            vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, m_aaQueryPool, m_currentFrame * 4);
            m_aaPassStamped[m_currentFrame] = true;
        }

        VkRenderPassBeginInfo rpInfo{};
        rpInfo.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rpInfo.renderPass        = taa ? m_taaHistoryRenderPass : m_aaRenderPass;
        rpInfo.framebuffer       = fb;
        // Tamano de VENTANA, no de render: este pass es justo el que baja de la
        // resolucion interna a la de presentacion.
        rpInfo.renderArea.extent = effectiveViewport();
        rpInfo.renderArea.offset = {0, 0};
        rpInfo.clearValueCount   = 0;   // los attachments son DONT_CARE

        vkCmdBeginRenderPass(cmd, &rpInfo, VK_SUBPASS_CONTENTS_INLINE);

        VkViewport viewport{};
        viewport.width    = (float)effectiveViewport().width;
        viewport.height   = (float)effectiveViewport().height;
        viewport.minDepth = 0.0f;
        viewport.maxDepth = 1.0f;
        vkCmdSetViewport(cmd, 0, 1, &viewport);

        VkRect2D scissor{};
        scissor.extent = effectiveViewport();
        vkCmdSetScissor(cmd, 0, 1, &scissor);

        switch (m_aaActiveMode)
        {
        case AaMode::Fxaa:
        {
            FxaaPush push{};
            // invRes de la imagen que se MUESTREA. En FXAA la intermedia tiene el
            // tamano de la ventana, pero se toma de m_renderExtent igualmente
            // para que el shader no dependa de que ambos coincidan.
            push.invResX          = 1.0f / (float)m_renderExtent.width;
            push.invResY          = 1.0f / (float)m_renderExtent.height;
            push.subpix           = m_fxaaSubpix;
            push.edgeThreshold    = m_fxaaEdgeThreshold;
            push.edgeThresholdMin = m_fxaaEdgeThresholdMin;

            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_fxaaPipeline);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_fxaaPipelineLayout,
                                    0, 1, &m_aaSets[m_currentFrame], 0, nullptr);
            vkCmdPushConstants(cmd, m_fxaaPipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT,
                               0, sizeof(push), &push);
            break;
        }
        case AaMode::Ssaa:
        {
            SsaaPush push{};
            push.invSrcX = 1.0f / (float)m_renderExtent.width;
            push.invSrcY = 1.0f / (float)m_renderExtent.height;
            // Una muestra por texel de origen y por eje: a factor 2 son los 4
            // texeles que caen dentro del pixel de destino, que es exactamente el
            // promedio que define el supersampling.
            push.taps    = (int32_t)std::lround((double)m_ssaaFactor);
            if (push.taps < 1) push.taps = 1;

            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_ssaaPipeline);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_ssaaPipelineLayout,
                                    0, 1, &m_aaSets[m_currentFrame], 0, nullptr);
            vkCmdPushConstants(cmd, m_ssaaPipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT,
                               0, sizeof(push), &push);
            break;
        }
        default:   // AaMode::Taa
        {
            TaaPush push{};
            // De clip de este frame a clip del anterior, los dos SIN jitter: es
            // la transformacion geometrica pura, el jitter es ruido de muestreo y
            // meterlo aqui desplazaria el historial medio pixel cada frame.
            push.reproject    = m_taaPrevViewProj * glm::inverse(m_taaCurrViewProj);
            push.invResX      = 1.0f / (float)effectiveViewport().width;
            push.invResY      = 1.0f / (float)effectiveViewport().height;
            push.feedback     = m_taaFeedback;
            push.historyValid = m_taaHistoryValid ? 1 : 0;

            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_taaPipeline);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_taaPipelineLayout,
                                    0, 1, &m_taaSets[m_currentFrame], 0, nullptr);
            vkCmdPushConstants(cmd, m_taaPipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT,
                               0, sizeof(push), &push);
            break;
        }
        }

        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdEndRenderPass(cmd);

        if (m_timestampsSupported)
            vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, m_aaQueryPool, m_currentFrame * 4 + 1);

        if (taa)
        {
            // A partir del segundo frame ya hay historial que acumular, y la
            // view-proj de este frame pasa a ser la "anterior" del siguiente.
            m_taaHistoryValid = true;
            m_taaPrevViewProj = m_taaCurrViewProj;
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
        if (v == m_ssaaFactor) return;
        m_ssaaFactor = v;
        // Solo cambia el tamano de los targets cuando SSAA es el modo activo.
        if (aaMode() == AaMode::Ssaa) m_aaResourcesDirty = true;
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

        if (m_aaActiveMode == AaMode::Ssaa && m_ssaaFactor > 1.0f)
        {
            VkPhysicalDeviceProperties props{};
            vkGetPhysicalDeviceProperties(m_gpu.physicalDevice(), &props);
            // maxFramebufferWidth/Height nunca son menores que maxImageDimension2D,
            // asi que con recortar por este limite basta para los dos.
            const uint32_t limit = props.limits.maxImageDimension2D;

            const uint32_t w = (uint32_t)std::lround(effectiveViewport().width  * (double)m_ssaaFactor);
            const uint32_t h = (uint32_t)std::lround(effectiveViewport().height * (double)m_ssaaFactor);
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
            // Los tres compute no dependen de las muestras, pero se van con
            // ellos: createComputePipelines los rehace de una pasada y volver a
            // crearlos encima del handle viejo si que seria una fuga.
            &m_boneEvalPipeline, &m_boneHierarchyPipeline, &m_skinningPipeline,
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
        createComputePipelines();

        // El pipeline de composicion vive en createBloomPipelines, que ademas
        // crea layouts y pools; aqui solo hace falta el pipeline, asi que se
        // rehace a mano con el mismo codigo que usa aquella.
        recreateCompositePipeline();
        // Comparte pass con la composicion: si cambian las muestras, su pipeline
        // deja de ser compatible igual que el de aquella.
        m_uiBatch.recreatePipeline(m_gpu, m_compositeRenderPass, m_aaSampleCount);

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
