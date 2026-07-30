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

        // Fase 1: lo minimo para poder presentar un frame (splash incluido).
        // El auto-fit de cámara y los recursos de escena (pipelines, shadow,
        // compute, descriptores independientes de la UI como offscreen, mallas)
        // viven en initSceneResources porque dependen de `meshes` o de
        // recursos creados ahí mismo.
        m_gpu.init(window.getNativeWindow());
        createSwapChain(window);

        createImageViews();
        createDepthResources();
        createOffscreenRenderPass();
        Gizmos::init(m_gpu, m_offscreenRenderPass, m_swapChainFormat);
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
            UiLayer::InitInfo info{};
            info.window         = window.getNativeWindow();
            info.instance       = m_gpu.instance();
            info.physicalDevice = m_gpu.physicalDevice();
            info.device         = m_gpu.device();
            info.queueFamily    = m_gpu.graphicsFamily();
            info.queue          = m_gpu.graphicsQueue();
            info.imageCount     = (uint32_t)m_swapChainImages.size();
            info.renderPass     = m_renderPass;
            m_ui->initUi(info);
        }
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
        createPipeline();
        createShadowResources();
        createComputePipelines();
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
        vkDestroyImage(m_gpu.device(), m_shadowImage, nullptr);
        vkFreeMemory(m_gpu.device(), m_shadowMemory, nullptr);
        vkDestroyFramebuffer(m_gpu.device(), m_shadowFramebuffer, nullptr);
        vkDestroyPipeline(m_gpu.device(), m_skinnedGfxPipeline, nullptr);
        vkDestroyPipeline(m_gpu.device(), m_skinnedWireframePipeline, nullptr);
        vkDestroyPipeline(m_gpu.device(), m_skinnedOutlinePipeline, nullptr);
        vkDestroyPipeline(m_gpu.device(), m_skinnedOutlineWirePipeline, nullptr);
        vkDestroyPipeline(m_gpu.device(), m_shadowPipeline, nullptr);
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
        printf("destroy render items OK\n"); fflush(stdout);
        m_gpu.shutdown();
    }

    void Renderer::initSkybox(const std::array<std::string, 6>& facePaths)
    {
        m_skybox.init(m_gpu, m_offscreenRenderPass, m_swapChainFormat, facePaths);
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

    Renderer::Frustum Renderer::frustumFromViewProj(const glm::mat4& m)
    {
        // glm es column-major: m[col][row]. Las filas de la matriz, que es lo
        // que necesita Gribb-Hartmann, hay que componerlas a mano.
        const glm::vec4 r0(m[0][0], m[1][0], m[2][0], m[3][0]);
        const glm::vec4 r1(m[0][1], m[1][1], m[2][1], m[3][1]);
        const glm::vec4 r2(m[0][2], m[1][2], m[2][2], m[3][2]);
        const glm::vec4 r3(m[0][3], m[1][3], m[2][3], m[3][3]);

        Frustum f;
        f.planes[0] = r3 + r0;  // izquierda
        f.planes[1] = r3 - r0;  // derecha
        f.planes[2] = r3 + r1;  // abajo
        f.planes[3] = r3 - r1;  // arriba
        // Cercano por el convenio de OpenGL (r3 + r2) y NO por el de Vulkan
        // (r2 a secas), a propósito: este motor mezcla los dos rangos de
        // profundidad (el editor arma su proyección con glm::perspective, que
        // sin GLM_FORCE_DEPTH_ZERO_TO_ONE da z=[-1,1]; CameraComponent y la luz
        // usan *RH_ZO, que da z=[0,1]). Sobre una matriz ZO, r3+r2 describe un
        // plano algo por DETRÁS del cercano real: recorta menos de lo que
        // podría, pero nunca descarta algo que se vería. Al revés — r2 sobre
        // una matriz [-1,1] — se comería la mitad cercana de la escena, y el
        // síntoma serían objetos que desaparecen al acercarse la cámara.
        f.planes[4] = r3 + r2;  // cercano
        f.planes[5] = r3 - r2;  // lejano

        // Normalizar: sin esto, dot(n,c)+d no es una distancia y el radio
        // proyectado de la AABB no sería comparable con ella.
        for (glm::vec4& p : f.planes)
        {
            const float len = glm::length(glm::vec3(p));
            if (len > 0.0f) p /= len;
        }
        return f;
    }

    bool Renderer::aabbVisible(const Frustum& frustum,
                               const glm::vec3& localMin,
                               const glm::vec3& localMax,
                               const glm::mat4& model)
    {
        // Centro + semiejes en vez de las 8 esquinas: el test por plano sale en
        // dos productos escalares en lugar de ocho.
        const glm::vec3 localCenter = (localMin + localMax) * 0.5f;
        const glm::vec3 localExtent = (localMax - localMin) * 0.5f;

        const glm::vec3 center = glm::vec3(model * glm::vec4(localCenter, 1.0f));

        // Semiejes de la AABB que envuelve a la caja ya transformada. El valor
        // absoluto de la 3x3 es lo que convierte una rotación en "cuánto crece
        // la caja alineada a ejes"; con escalado no uniforme sale bien igual.
        const glm::mat3 rs = glm::mat3(model);
        const glm::vec3 extent(
            std::abs(rs[0][0]) * localExtent.x + std::abs(rs[1][0]) * localExtent.y + std::abs(rs[2][0]) * localExtent.z,
            std::abs(rs[0][1]) * localExtent.x + std::abs(rs[1][1]) * localExtent.y + std::abs(rs[2][1]) * localExtent.z,
            std::abs(rs[0][2]) * localExtent.x + std::abs(rs[1][2]) * localExtent.y + std::abs(rs[2][2]) * localExtent.z);

        for (const glm::vec4& p : frustum.planes)
        {
            const glm::vec3 n(p);
            const float distance = glm::dot(n, center) + p.w;
            // Radio de la caja proyectado sobre la normal del plano: la esquina
            // que más sobresale hacia el lado positivo.
            const float radius = std::abs(n.x) * extent.x + std::abs(n.y) * extent.y + std::abs(n.z) * extent.z;
            if (distance + radius < 0.0f) return false; // entera del lado de fuera
        }
        return true;
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

    glm::mat4 Renderer::shadowLightSpaceMatrix() const
    {
        if (m_lights.empty()) return glm::mat4(1.0f);

        const glm::vec3 lightPos = glm::vec3(m_lights[0].position);
        glm::mat4 lightView = glm::lookAt(lightPos, glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
        glm::mat4 lightProj = glm::orthoRH_ZO(-350.0f, 350.0f, -350.0f, 350.0f, 1.0f, 2000.0f);
        lightProj[1][1] *= -1.0f;
        return lightProj * lightView;
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
        colorAtt.format         = m_swapChainFormat;
        colorAtt.samples        = VK_SAMPLE_COUNT_1_BIT;
        colorAtt.loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
        colorAtt.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
        colorAtt.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        colorAtt.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        colorAtt.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
        colorAtt.finalLayout    = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkAttachmentReference colorRef{};
        colorRef.attachment = 0;
        colorRef.layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        VkAttachmentDescription depthAtt{};
        depthAtt.format         = VK_FORMAT_D32_SFLOAT;
        depthAtt.samples        = VK_SAMPLE_COUNT_1_BIT;
        depthAtt.loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depthAtt.storeOp        = VK_ATTACHMENT_STORE_OP_DONT_CARE;
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

        // Dependencias: garantizan que la UI puede leer la textura cuando el pass acaba
        VkSubpassDependency deps[2]{};
        deps[0].srcSubpass    = VK_SUBPASS_EXTERNAL;
        deps[0].dstSubpass    = 0;
        deps[0].srcStageMask  = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        deps[0].dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        deps[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        deps[0].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

        deps[1].srcSubpass    = 0;
        deps[1].dstSubpass    = VK_SUBPASS_EXTERNAL;
        deps[1].srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        deps[1].dstStageMask  = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        deps[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        deps[1].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

        VkAttachmentDescription attachments[] = { colorAtt, depthAtt };
        VkRenderPassCreateInfo rpInfo{};
        rpInfo.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        rpInfo.attachmentCount = 2;
        rpInfo.pAttachments    = attachments;
        rpInfo.subpassCount    = 1;
        rpInfo.pSubpasses      = &subpass;
        rpInfo.dependencyCount = 2;
        rpInfo.pDependencies   = deps;

        if (vkCreateRenderPass(m_gpu.device(), &rpInfo, nullptr, &m_offscreenRenderPass) != VK_SUCCESS)
            throw std::runtime_error("failed to create offscreen render pass!");

        printf("offscreen render pass OK\n"); fflush(stdout);
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
            // Mismas tres guardas que el bucle de dibujo, en el mismo orden:
            // entrada borrada, upload en vuelo y frustum. Un objeto sin contorno
            // porque está fuera de cámara es lo correcto — el objeto tampoco se
            // ha dibujado.
            bool visible = gpu && gpu->uploadTicket <= m_lastCompletedTicket;
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
                    m_wireframeMode ? m_outlineWirePipeline : m_outlinePipeline);
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
            if (m_skinnedVisible[m_outlineSkinnedIndex])
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
                        m_wireframeMode ? m_skinnedOutlineWirePipeline : m_skinnedOutlinePipeline);
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
        // es que todos los objetos sean visibles en los DOS passes, de ahí el
        // factor 2. El cursor arranca a 0: sombras escriben delante, escena
        // detrás.
        ensureInstanceCapacity((uint32_t)m_objects.size() * 2);
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

        recordComputePass(m_commandBuffers[m_currentFrame]);
        recordShadowPass(m_commandBuffers[m_currentFrame]);

        // ── Pass 1: escena 3D → offscreen ────────────────────────────────────────
        {
            VkClearValue clearValues[2];
            clearValues[0].color        = {0.0f, 0.0f, 0.0f, 1.0f};
            clearValues[1].depthStencil = {1.0f, 0};

            VkRenderPassBeginInfo rpInfo{};
            rpInfo.sType               = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
            rpInfo.renderPass          = m_offscreenRenderPass;
            rpInfo.framebuffer         = m_offscreenFramebuffer[m_currentFrame];
            rpInfo.renderArea.extent   = m_swapChainExtent;
            rpInfo.renderArea.offset   = {0, 0};
            rpInfo.clearValueCount     = 2;
            rpInfo.pClearValues        = clearValues;

            vkCmdBeginRenderPass(m_commandBuffers[m_currentFrame], &rpInfo, VK_SUBPASS_CONTENTS_INLINE);

            VkViewport viewport{};
            viewport.width    = (float)m_swapChainExtent.width;
            viewport.height   = (float)m_swapChainExtent.height;
            viewport.minDepth = 0.0f;
            viewport.maxDepth = 1.0f;
            vkCmdSetViewport(m_commandBuffers[m_currentFrame], 0, 1, &viewport);

            VkRect2D scissor{};
            scissor.extent = m_swapChainExtent;
            vkCmdSetScissor(m_commandBuffers[m_currentFrame], 0, 1, &scissor);

            vkCmdBindPipeline(m_commandBuffers[m_currentFrame], VK_PIPELINE_BIND_POINT_GRAPHICS,
                m_wireframeMode ? m_wireframePipeline : m_pipeline);
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
                bool visible = gpu && gpu->uploadTicket <= m_lastCompletedTicket;
                // Fuera de cámara: no gasta ni slot en el SSBO. Los objetos sin
                // AABB (mesh vacío) pasan siempre.
                if (visible && gpu->hasBounds &&
                    !aabbVisible(camFrustum, gpu->aabbMin, gpu->aabbMax, obj.transform))
                {
                    visible = false;
                }
                m_batchCandidates.push_back({ obj.sharedIndex, visible, &obj.transform });
            }

            // El pass de sombras ya ha escrito su parte del buffer: sus
            // transforms van delante y los de aquí detrás, con el cursor como
            // base de los firstInstance.
            const uint32_t instanceBase = m_instanceCursor;
            glm::mat4* dst = (glm::mat4*)m_instanceMapped[m_currentFrame] + instanceBase;
            m_instanceCursor += buildInstanceBatches(m_batchCandidates.data(), m_batchCandidates.size(),
                dst, m_instanceCapacity[m_currentFrame] - instanceBase, instanceBase, m_instanceBatches);

            // Set 1 una sola vez para todo el pass: el SSBO no cambia entre
            // draws, y el pipeline skinned de abajo comparte layout, así que
            // sigue bindeado y válido también para él.
            vkCmdBindDescriptorSets(m_commandBuffers[m_currentFrame], VK_PIPELINE_BIND_POINT_GRAPHICS,
                m_pipelineLayout, 1, 1, &m_instanceDescSets[m_currentFrame], 0, nullptr);

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
                    m_wireframeMode ? m_skinnedWireframePipeline : m_skinnedGfxPipeline);

                for (size_t si = 0; si < m_skinnedObjects.size(); si++)
                {
                    // Borrado, en vuelo o fuera de cámara: la decisión ya la tomó
                    // el culling del principio del frame, la misma que decidió si
                    // se le despachaba el compute.
                    if (!m_skinnedVisible[si]) continue;
                    SkinnedRenderObject& sobj = m_skinnedObjects[si];
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
                        vkCmdBindDescriptorSets(m_commandBuffers[m_currentFrame],
                            VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout,
                            0, 1, &mgfx.descSets[m_currentFrame], 0, nullptr);
                        vkCmdPushConstants(m_commandBuffers[m_currentFrame], m_pipelineLayout,
                            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                            0, sizeof(PushData), &push);
                        vkCmdDrawIndexed(m_commandBuffers[m_currentFrame],
                            sm.indexCount, 1, sm.indexStart, 0, 0);
                    }
                }
            }

            // Contorno del objeto seleccionado: después de toda la geometría
            // (necesita el depth buffer completo para que el casco solo asome
            // por el borde) y antes del skybox.
            recordSelectionOutline(m_commandBuffers[m_currentFrame], camFrustum);

            // Proyección compartida por skybox y gizmos (mismo pass, misma
            // cámara que el culling de arriba). El Y-flip ya viene aplicado
            // desde currentFrameCamera().
            const glm::mat4 proj = fc.proj;

            // Skybox — fullscreen quad, depth LEQUAL sin escritura (al final del pass).
            // Omitido en wireframe: el fondo ya es negro sólido (clearValue por defecto).
            if (!m_wireframeMode && m_skybox.isInitialized()) {
                glm::mat4 rotView    = glm::mat4(glm::mat3(fc.view)); // sin traslación
                glm::mat4 invViewProj = glm::inverse(proj * rotView);
                m_skybox.draw(m_commandBuffers[m_currentFrame], invViewProj);
            }

            // Gizmos — mismo pass, tras el skybox, respetando el depth test de la escena.
            {
                Gizmos::draw(m_commandBuffers[m_currentFrame], proj * fc.view, m_currentFrame);
            }

            vkCmdEndRenderPass(m_commandBuffers[m_currentFrame]);
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
            if (m_ui) m_ui->recordUi(m_commandBuffers[m_currentFrame]);
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
            blit.srcOffsets[1]  = { (int32_t)m_swapChainExtent.width, (int32_t)m_swapChainExtent.height, 1 };
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

        // 6. Multisampling — sin MSAA por ahora
        VkPipelineMultisampleStateCreateInfo multisampleInfo{};
        multisampleInfo.sType                   = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisampleInfo.rasterizationSamples    = VK_SAMPLE_COUNT_1_BIT;

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
        VkDescriptorSetLayout setLayouts[] = { m_descriptorSetLayout, m_instanceDescLayout };

        VkPipelineLayoutCreateInfo layoutInfo{};
        layoutInfo.sType                    = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layoutInfo.setLayoutCount           = 2;
        layoutInfo.pSetLayouts              = setLayouts;
        layoutInfo.pushConstantRangeCount   = 1;
        layoutInfo.pPushConstantRanges      = &pushRange;
        if(vkCreatePipelineLayout(m_gpu.device(), &layoutInfo, nullptr, &m_pipelineLayout) != VK_SUCCESS)
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

        VkDescriptorSetLayoutBinding bindings[] = { uboBinding, samplerBinding, samplerNormal, shadowBinding, ormBinding };

        VkDescriptorSetLayoutCreateInfo layoutInfo{};
        layoutInfo.sType            = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layoutInfo.bindingCount     = 5;
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
        outBatches.clear();
        if (count == 0 || outCapacity == 0 || outTransforms == nullptr) return 0;

        // Tabla sharedIndex -> posición en outBatches. Los sharedIndex son
        // índices densos y pequeños de la caché, así que una tabla plana evita
        // el hash de un unordered_map (que en escenas de miles de objetos se
        // comía justo lo que este agrupado viene a ahorrar).
        int maxShared = -1;
        for (size_t i = 0; i < count; i++)
            if (candidates[i].visible && candidates[i].sharedIndex > maxShared)
                maxShared = candidates[i].sharedIndex;
        if (maxShared < 0) return 0; // no hay nada visible
        std::vector<int> slotOf((size_t)maxShared + 1, -1);

        // Pasada 1: un grupo por sharedIndex, en orden de primera aparición.
        for (size_t i = 0; i < count; i++)
        {
            const BatchCandidate& c = candidates[i];
            if (!c.visible || c.sharedIndex < 0 || c.transform == nullptr) continue;
            int& slot = slotOf[(size_t)c.sharedIndex];
            if (slot < 0)
            {
                slot = (int)outBatches.size();
                outBatches.push_back({ c.sharedIndex, 0, 0 });
            }
            outBatches[(size_t)slot].instanceCount++;
        }

        // Offsets contiguos. Si un grupo no cabe entero se recorta a lo que
        // queda y los siguientes se quedan a cero: mejor perder objetos que
        // escribir fuera del buffer.
        uint32_t written = 0;
        for (auto& b : outBatches)
        {
            const uint32_t room = outCapacity - written;
            if (b.instanceCount > room) b.instanceCount = room;
            b.firstInstance = firstInstanceBase + written;
            written += b.instanceCount;
        }

        // Pasada 2: transforms contiguos por grupo, en el orden de los
        // candidatos. cursor lleva cuántos se han escrito ya de cada grupo, que
        // es también el hueco relativo dentro de su rango.
        std::vector<uint32_t> cursor(outBatches.size(), 0);
        for (size_t i = 0; i < count; i++)
        {
            const BatchCandidate& c = candidates[i];
            if (!c.visible || c.sharedIndex < 0 || c.transform == nullptr) continue;
            const size_t slot = (size_t)slotOf[(size_t)c.sharedIndex];
            if (cursor[slot] >= outBatches[slot].instanceCount) continue; // grupo recortado
            const uint32_t dst = (outBatches[slot].firstInstance - firstInstanceBase) + cursor[slot];
            outTransforms[dst] = *c.transform;
            cursor[slot]++;
        }

        // Los grupos que se quedaron sin sitio no deben llegar como draws de 0
        // instancias.
        outBatches.erase(std::remove_if(outBatches.begin(), outBatches.end(),
                             [](const InstanceBatch& b) { return b.instanceCount == 0; }),
                         outBatches.end());
        return written;
    }

    void Renderer::createDescriptorPool()
    {
        uint32_t n = (uint32_t)((m_objects.size() + 128) * MAX_FRAMES);
        VkDescriptorPoolSize poolSizes[2]{};
        poolSizes[0].type            = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        poolSizes[0].descriptorCount = n;
        poolSizes[1].type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        poolSizes[1].descriptorCount = n * 4;   // diffuse + normal map + shadow + orm

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

            VkWriteDescriptorSet writes[5]{};
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

            vkUpdateDescriptorSets(m_gpu.device(), 5, writes, 0, nullptr);
        }
    }

    void Renderer::updateUniformBuffer(uint32_t frameIndex)
    {
        // Cámara del CameraComponent en Play, la del editor en edición. El
        // Y-flip de Vulkan ya viene aplicado desde currentFrameCamera().
        const FrameCamera fc = currentFrameCamera();
        UniformBufferObject ubo{};
        ubo.view = fc.view;
        ubo.proj = fc.proj;
        ubo.numLights = std::min((int)m_lights.size(), MAX_LIGHTS);
        for(int i = 0; i < ubo.numLights; i++)
        {
            ubo.lights[i] = m_lights[i];
        }
        
        ubo.viewPos  = glm::vec4(fc.eye, 1.0f);
        if (!m_lights.empty())
        {
            ubo.lightSpaceMatrix = shadowLightSpaceMatrix();
        }

        memcpy(m_uniformBuffersMapped[frameIndex], &ubo, sizeof(ubo));        
    }

    void Renderer::createDepthResources()
    {
        VkFormat depthFormat = VK_FORMAT_D32_SFLOAT;

        VkImageCreateInfo imageInfo{};
        imageInfo.sType             = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType         = VK_IMAGE_TYPE_2D;
        imageInfo.format            = depthFormat;
        imageInfo.extent            = { m_swapChainExtent.width, m_swapChainExtent.height, 1 };
        imageInfo.mipLevels         = 1;
        imageInfo.arrayLayers       = 1;
        imageInfo.samples           = VK_SAMPLE_COUNT_1_BIT;
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
        // 1. Imagen depth para shadow map
        m_res.createImage(SHADOW_SIZE, SHADOW_SIZE, VK_FORMAT_D32_SFLOAT, VK_IMAGE_TILING_OPTIMAL, 
            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, m_shadowImage, m_shadowMemory);

        // 2. Image view (depth aspect)
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType                          = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image                          = m_shadowImage;
        viewInfo.viewType                       = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format                         = VK_FORMAT_D32_SFLOAT;
        viewInfo.subresourceRange.aspectMask    = VK_IMAGE_ASPECT_DEPTH_BIT;
        viewInfo.subresourceRange.layerCount    = 1;
        viewInfo.subresourceRange.levelCount    = 1;    
        if(vkCreateImageView(m_gpu.device(), &viewInfo, nullptr, &m_shadowView) != VK_SUCCESS)
        {
            throw std::runtime_error("failed to create shadow image view!");
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

         // 5. Framebuffer
         VkFramebufferCreateInfo fbInfo{};
        fbInfo.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fbInfo.renderPass      = m_shadowRenderPass;
        fbInfo.attachmentCount = 1;
        fbInfo.pAttachments    = &m_shadowView;
        fbInfo.width           = SHADOW_SIZE;
        fbInfo.height          = SHADOW_SIZE;
        fbInfo.layers          = 1;
        if (vkCreateFramebuffer(m_gpu.device(), &fbInfo, nullptr, &m_shadowFramebuffer) != VK_SUCCESS)
        {
            throw std::runtime_error("failed to create shadow framebuffer!");
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

        // Sin push constants: shadow.vert saca el model matrix del SSBO de
        // instancias (set 1) por gl_InstanceIndex, igual que triangle.vert, y
        // por este pass solo pasan objetos estáticos agrupados.
        VkDescriptorSetLayout setLayouts[] = { m_descriptorSetLayout, m_instanceDescLayout };

        VkPipelineLayoutCreateInfo layoutInfo{};
        layoutInfo.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layoutInfo.setLayoutCount         = 2;
        layoutInfo.pSetLayouts            = setLayouts;
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

        vkDestroyShaderModule(m_gpu.device(), vertModule, nullptr);
    }

    void Renderer::recordShadowPass(VkCommandBuffer cmd)
    {
        VkClearValue clearDepth{};
        clearDepth.depthStencil = { 1.0f, 0 };

        VkRenderPassBeginInfo renderPassInfo{};
        renderPassInfo.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        renderPassInfo.renderPass        = m_shadowRenderPass;
        renderPassInfo.framebuffer       = m_shadowFramebuffer;
        renderPassInfo.renderArea.extent = { SHADOW_SIZE, SHADOW_SIZE };
        renderPassInfo.clearValueCount   = 1;
        renderPassInfo.pClearValues      = &clearDepth;

        vkCmdBeginRenderPass(cmd, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_shadowPipeline);

        
        VkViewport vp {0.0f, 0.0f, (float)SHADOW_SIZE, (float)SHADOW_SIZE, 0.0f, 1.0f};
        VkRect2D sc {{0,0}, {SHADOW_SIZE, SHADOW_SIZE}};
        vkCmdSetViewport(cmd, 0, 1, &vp);
        vkCmdSetScissor(cmd, 0, 1, &sc);

        // Culling por el frustum de la LUZ, no por el de la cámara: un objeto
        // que la cámara no ve puede seguir proyectando sombra sobre lo que sí
        // se ve. Con el frustum de la cámara desaparecerían esas sombras.
        //
        // Sin luces no hay matriz que extraer (shadowLightSpaceMatrix devuelve
        // la identidad, cuyo frustum es el cubo unidad y culearía casi todo):
        // en ese caso se graba el pass entero como antes.
        const bool    cullByLight  = !m_lights.empty();
        const Frustum lightFrustum = cullByLight ? frustumFromViewProj(shadowLightSpaceMatrix())
                                                 : Frustum{};

        // Mismas guardas por objeto que el pass principal, con el frustum de la
        // luz. El agrupado es independiente del de la cámara: los conjuntos
        // visibles no coinciden, así que cada pass escribe su propio rango del
        // SSBO (este va primero, el de la escena detrás).
        m_batchCandidates.clear();
        m_batchCandidates.reserve(m_objects.size());
        for(auto& obj : m_objects)
        {
            const SharedGpuMesh* gpu = m_sharedMeshes.get(obj.sharedIndex);
            // !gpu: borrado desde el editor. En vuelo: no debe proyectar sombra
            // si todavía no es visible, o habría una sombra flotando sin objeto
            // que la eche.
            bool visible = gpu && gpu->uploadTicket <= m_lastCompletedTicket;
            // Fuera del volumen que cubre el shadow map: su sombra no cabría en
            // la textura de todos modos.
            if (visible && cullByLight && gpu->hasBounds &&
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

        vkCmdEndRenderPass(cmd);
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
        if (vkCreateDescriptorSetLayout(m_gpu.device(), &dslInfo, nullptr, &m_computeDescLayout) != VK_SUCCESS)
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
        if (vkCreatePipelineLayout(m_gpu.device(), &plInfo, nullptr, &m_computePipelineLayout) != VK_SUCCESS)
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
        if (vkCreateDescriptorPool(m_gpu.device(), &poolInfo, nullptr, &m_computeDescPool) != VK_SUCCESS)
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
            ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

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

                VkWriteDescriptorSet gw[5]{};
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

                vkUpdateDescriptorSets(m_gpu.device(), 5, gw, 0, nullptr);
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
            // Imagen color offscreen
            m_res.createImage(
                m_swapChainExtent.width, m_swapChainExtent.height,
                m_swapChainFormat,
                VK_IMAGE_TILING_OPTIMAL,
                // TRANSFER_SRC: origen del blit al swapchain en headless.
                VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                    VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                m_offscreenImage[i], m_offscreenMemory[i]);

            m_res.createTextureImageView(m_offscreenImage[i], m_offscreenView[i], m_swapChainFormat);

            // Framebuffer offscreen: color + depth compartido
            VkImageView atts[] = { m_offscreenView[i], m_depthImageView };
            VkFramebufferCreateInfo fbInfo{};
            fbInfo.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            fbInfo.renderPass      = m_offscreenRenderPass;
            fbInfo.attachmentCount = 2;
            fbInfo.pAttachments    = atts;
            fbInfo.width           = m_swapChainExtent.width;
            fbInfo.height          = m_swapChainExtent.height;
            fbInfo.layers          = 1;
            if (vkCreateFramebuffer(m_gpu.device(), &fbInfo, nullptr, &m_offscreenFramebuffer[i]) != VK_SUCCESS)
                throw std::runtime_error("failed to create offscreen framebuffer!");

            // Registrar la textura en la capa de UI para obtener el
            // VkDescriptorSet. En headless nadie la muestrea: el descriptor
            // set se queda nulo y destroyOffscreenImages ya comprueba antes
            // de liberarlo.
            if (!m_headless && m_ui)
                m_offscreenDescSet[i] = m_ui->registerUiTexture(m_offscreenSampler, m_offscreenView[i]);
        }
        printf("offscreen images OK\n"); fflush(stdout);
    }

    void Renderer::destroyOffscreenImages()
    {
        for (int i = 0; i < MAX_FRAMES; i++)
        {
            if (m_offscreenDescSet[i] && m_ui)
            {
                m_ui->unregisterUiTexture(m_offscreenDescSet[i]);
                m_offscreenDescSet[i] = VK_NULL_HANDLE;
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

}
