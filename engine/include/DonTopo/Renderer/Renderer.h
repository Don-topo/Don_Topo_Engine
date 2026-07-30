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
#include "DonTopo/Renderer/GpuDevice.h"
#include "DonTopo/Renderer/GpuResources.h"
#include "DonTopo/Renderer/SharedGpuMesh.h"
#include "DonTopo/Renderer/DeferredDelete.h"
#include "DonTopo/Renderer/TransferBatch.h"
#include "DonTopo/Renderer/AsyncAssetLoader.h"
#include "DonTopo/Renderer/UiLayer.h"
#include "DonTopo/Renderer/Skybox.h"
#include "DonTopo/Renderer/SplashScreen.h"
#include <array>

namespace DonTopo {

    class Window;
    class GameObject;
    class PhysicsManager;
    class AudioManager;
    class Scene;
    class ScriptManager;

    class Renderer {
        public:
            Renderer()                              = default;
            ~Renderer();
            Renderer(const Renderer&)               = delete;
            Renderer& operator=(const Renderer&)    = delete;
            void init(Window& window, const std::vector<Mesh>& meshes);
            // Fase 1 del arranque: lo minimo para presentar (device, swapchain,
            // render pass, framebuffers, command buffers, sync, capa de UI). No crea
            // pipelines de escena. La usa el runtime para poder dibujar el
            // splash antes de la carga pesada. init() la llama primero.
            void initPresentation(Window& window);
            // Fase 2: pipelines PBR/shadow/compute, offscreen, descriptor sets,
            // subida de mallas estaticas y auto-fit de camara (necesita meshes).
            // init() la llama despues.
            void initSceneResources(const std::vector<Mesh>& meshes);
            // Inicializa el splash sobre el render pass del swapchain (requiere
            // initPresentation ya llamado). false si el logo no carga: el caller
            // se salta el splash. Solo lo llama el runtime; el editor no.
            bool beginSplash(const std::string& logoPath);
            // Presenta un frame con solo el splash a alpha [0,1]. No-op si el
            // splash no se inicializo.
            void drawSplashFrame(float alpha);
            void drawFrame(Window& window);
            // Una vez por frame, desde el bucle principal, ANTES de drawFrame.
            // Es lo que hace avanzar la cola de destrucción diferida.
            void tickDeferredDeletes();
            void shutdown();
            void setCamera(const Camera& camera);
            void notifyResize() { m_framebufferResized = true; }
            void setWireframeMode(bool enabled) { m_wireframeMode = enabled; }
            bool isWireframeMode() const { return m_wireframeMode; }
            // Contorno de seleccion: indices del objeto resaltado en m_objects y
            // en m_skinnedObjects (los mismos que guarda GameObject en
            // staticRenderIndex/skinnedRenderIndex), -1 para "ninguno". Solo lo
            // llama el editor, una vez por frame; el runtime no tiene seleccion
            // y con el default (-1, -1) no se dibuja ningun contorno. El objeto
            // resaltado sigue sujeto al mismo culling que el resto: fuera del
            // frustum no se dibuja nada.
            void setOutlineTarget(int staticIndex, int skinnedIndex)
            {
                m_outlineStaticIndex  = staticIndex;
                m_outlineSkinnedIndex = skinnedIndex;
            }
            // En headless no hay editor que pulse Play: el runtime arranca
            // jugando desde el frame 0. Esto es además lo que hace que
            // currentFrameCamera() elija el CameraComponent de la escena en
            // vez de la cámara de vuelo del editor.
            bool isPlaying() const;
            // Modo runtime: ni UI ni paneles. Solo tiene efecto si se
            // llama ANTES de initPresentation() (o de init(), que la llama) —
            // createOffscreenImages y el arranque de la UI leen el flag
            // durante esa inicialización.
            void setHeadless(bool headless) { m_headless = headless; }
            // Capa de UI, no-propietaria: es el editor quien posee al Renderer
            // y se registra aquí. nullptr (runtime) = no hay pass de UI.
            // Debe fijarse ANTES de initPresentation(), que es quien la
            // arranca.
            void setUiLayer(UiLayer* ui) { m_ui = ui; }
            // currentFrameCamera() necesita preguntarle a la escena por su
            // cámara cada frame (Scene::findCamera es la única fuente de
            // verdad).
            void setScene(Scene* scene) { m_scene = scene; }

            // Aspect del render target. Público porque el gizmo de frustum
            // (ViewportPanel) tiene que usar EXACTAMENTE el mismo que usará la
            // proyección de Play, o dibujaría un encuadre que no se corresponde.
            float viewportAspect() const
            {
                return m_swapChainExtent.height > 0
                    ? (float)m_swapChainExtent.width / (float)m_swapChainExtent.height
                    : 1.0f;
            }
            void setSceneRoot(GameObject* root) { m_sceneRoot = root; }
            // Libera mesh/skinnedMesh/texturas en GPU de node y todo su subárbol
            // (llamado por EditorUI justo antes de borrar el nodo del scene graph).
            void removeGameObject(GameObject* node);
            // Inverso de removeGameObject: sube a GPU los meshes (estático o
            // skinned) de node y su subárbol que aún no estén registrados
            // (staticRenderIndex/skinnedRenderIndex < 0). Usado tras
            // reconstruir un subárbol desde JSON — reloadSceneFromJson (toda
            // la escena) y CreateGameObjectCommand/DeleteGameObjectCommand en
            // Command.cpp (un solo subárbol).
            void registerGameObject(GameObject* node);
            // Quita solo el componente Mesh de go (no borra el GameObject ni sus
            // otros componentes). No-op si go es nullptr o no tiene mesh.
            void removeMeshComponent(GameObject* go);
            enum class TextureSlot { Diffuse, Normal, MetallicRoughness };
            // Sustituye la textura del slot indicado por el checkerboard
            // "missing" (mismo generador que createTextureImage usa cuando no
            // hay path/bytes). No-op si renderIndex está fuera de rango.
            // Sincroniza con vkDeviceWaitIdle antes de tocar el descriptor
            // set (evita pisar un frame en vuelo). Solo cubre meshes
            // estáticos — no hay UI hoy que asigne meshes skinned.
            void replaceStaticTextureWithMissing(int renderIndex, TextureSlot slot);
            // facePaths: +X, -X, +Y, -Y, +Z, -Z (cualquier formato soportado por stb_image)
            void initSkybox(const std::array<std::string, 6>& facePaths);
            void setTransform(size_t objectIndex, const glm::mat4& transform)
            {
                if (objectIndex < m_objects.size())
                    m_objects[objectIndex].transform = transform;
            }
            void setLights(const std::vector<Light>& lights){ m_lights = lights; }
            // decoded: píxeles que el worker ya decodificó para este mesh (nullptr
            // en el camino síncrono). Encola todos los uploads en el batch del pump
            // actual y marca el objeto con el ticket vigente: no se dibuja hasta que
            // flushPendingUploads() lo envíe y su fence señale.
            int addSkinnedMesh(const SkinnedMesh& mesh, const std::vector<DecodedImage>* decoded = nullptr);
            // Rehace TODOS los recursos GPU del objeto skinned `index` a partir
            // de `mesh`, en el mismo slot (el skinnedRenderIndex del GameObject
            // no cambia). Necesario tras añadir o quitar clips: los keyframes
            // viven en SSBOs subidos una sola vez, y la GPU tendría la lista
            // vieja. Conserva transform, animTime y activeClip.
            void rebuildSkinnedMesh(int index, const SkinnedMesh& mesh);
            // Añade un mesh estático nuevo (buffers + texturas + descriptor set) y lo
            // registra en m_objects. Devuelve el índice para GameObject::staticRenderIndex.
            // decoded: mismo contrato que addSkinnedMesh (nullptr = camino síncrono).
            int addStaticMesh(const Mesh& mesh, const std::vector<DecodedImage>* decoded = nullptr);
            // Cierra y envía el batch del pump actual. Llamar UNA vez tras
            // procesar todos los resultados del frame.
            void flushPendingUploads();
            // true si hay uploads sin completar: un batch abierto en m_pendingBatch
            // o batches todavía en vuelo. Lo consume el runtime (Task 10) para saber
            // si aún debe seguir bombeando antes de dar la carga por terminada.
            bool hasPendingUploads() const;
            // Cierra el batch pendiente y BLOQUEA hasta que todos los uploads en
            // vuelo hayan completado y sido reclamados, de modo que los objetos
            // recién registrados sean visibles en ESTE frame. Uso reservado a las
            // transiciones síncronas raras iniciadas por el usuario (restore de
            // Play->Stop, undo/redo de un Create): ahí el stall de vkDeviceWaitIdle
            // es aceptable y reproduce la visibilidad inmediata previa a la carga
            // asíncrona. NO usar en el camino async de Load Scene (tiene su modal +
            // pump por frame; bloquear reintroduciría el stall que el modal evita).
            void flushUploadsAndWait();
            void updateAnimation(int index, float deltaTime);
            // Sink puro: fija el clip y el tiempo que el Animator ya ha
            // calculado en CPU. No avanza el tiempo — a diferencia de
            // updateAnimation, que sigue siendo el camino de los objetos SIN
            // AnimatorComponent. Los dos no se pisan: quien tiene Animator nunca
            // pasa por updateAnimation.
            void setAnimationState(int index, uint32_t clipIndex, float animTime);
            void setSkinnedTransform(int index, const glm::mat4& transform);

            // ── Frustum culling ──────────────────────────────────────────────
            // Seis planos en espacio de mundo, con la normal apuntando HACIA
            // DENTRO del volumen: un punto es visible si queda del lado
            // positivo de los seis. Cada plano es (nx, ny, nz, d) con la normal
            // normalizada, de forma que dot(n, p) + d es la distancia con signo.
            struct Frustum {
                glm::vec4 planes[6] = {};
            };
            // Extrae los planos de una matriz viewProj (Gribb-Hartmann). Asume
            // el rango de profundidad de Vulkan z=[0,1] — el plano cercano sale
            // de la fila 2 a secas, no de (fila3 + fila2) como en el convenio
            // de OpenGL. Con matrices *RH_ZO (las que usa este motor, ver
            // updateUniformBuffer) esto es lo correcto; con glm::perspective a
            // secas culearía de más por el lado cercano.
            static Frustum frustumFromViewProj(const glm::mat4& viewProj);
            // AABB en espacio LOCAL del mesh + su transform a mundo. Devuelve
            // false solo si la caja queda entera fuera de algún plano; es un
            // test conservador (puede dar true de más en las esquinas del
            // frustum, nunca false de menos, que sería un objeto desaparecido).
            static bool aabbVisible(const Frustum& frustum,
                                    const glm::vec3& localMin,
                                    const glm::vec3& localMax,
                                    const glm::mat4& model);
            // Radio de una esfera centrada en el ORIGEN LOCAL del modelo que
            // contiene la malla skinned en CUALQUIER pose de CUALQUIER clip. La
            // AABB en reposo no vale: el compute deforma los vértices y un brazo
            // levantado se sale de la caja, así que cullear con ella haría
            // desaparecer al personaje — el peor fallo posible aquí.
            //
            // No evalúa ninguna pose: acota hueso a hueso con los valores
            // EXTREMOS de las keys (alcance acumulado por la jerarquía × escala
            // acumulada, más el radio de la nube de vértices que arrastra cada
            // hueso medido en su propio espacio). Por eso la cota vale también
            // para los tiempos interpolados: mix() no sale del segmento entre
            // sus extremos y slerp() devuelve una rotación, que no cambia
            // normas. Es holgada a propósito — falso positivo sí, falso
            // negativo nunca.
            //
            // Devuelve 0 si no hay con qué acotar (sin huesos o sin vértices);
            // el llamante lo trata como "sin cota" y no culea.
            static float skinnedBoundRadius(const SkinnedMesh& mesh);
            // Matrices de luz de las N cascadas y sus distancias de corte. Las
            // comparten el UBO (para que el fragment shader muestree el shadow
            // map) y el culling del pass de sombras: si las dos se calcularan
            // por separado y una cambiara, se culearían objetos que el shadow
            // map sí necesita. Por eso se calculan UNA vez por frame en draw(),
            // antes de updateUniformBuffer y de recordCommandBuffer, y los dos
            // consumidores leen la caché de abajo.
            void computeCascades();

            // ── Draw batching por instancing ─────────────────────────────────
            // Un draw instanciado: todas las instancias del rango
            // [firstInstance, firstInstance + instanceCount) del SSBO de
            // transforms comparten la entrada compartida sharedIndex, así que
            // se dibujan con un solo vkCmdDrawIndexed.
            struct InstanceBatch {
                int      sharedIndex   = -1;
                uint32_t firstInstance = 0;
                uint32_t instanceCount = 0;
            };
            // Un objeto ya evaluado por el pass que lo va a dibujar. Las guardas
            // (entrada borrada, upload en vuelo) y el culling por AABB los
            // resuelve el llamante -es quien tiene la caché GPU y el frustum- y
            // llegan aquí resumidos en `visible`. El transform va por puntero: el
            // agrupado solo lo copia al SSBO, no lo guarda.
            struct BatchCandidate {
                int              sharedIndex = -1;
                bool             visible     = false;
                const glm::mat4* transform   = nullptr;
            };
            // Agrupa por sharedIndex los candidatos VISIBLES y deja sus
            // transforms contiguos por grupo en outTransforms (que apunta ya al
            // hueco del SSBO, con sitio para outCapacity matrices). El orden es
            // estable: los grupos salen por orden de primera aparición y dentro
            // de cada grupo se conserva el orden de los candidatos, de modo que
            // el resultado no baila entre frames.
            //
            // firstInstanceBase es el índice ABSOLUTO dentro del SSBO de la
            // primera matriz escrita: los dos passes comparten buffer y el
            // segundo escribe detrás del primero, así que sin base los
            // firstInstance del pass principal apuntarían al rango de sombras.
            //
            // Devuelve cuántas matrices se han escrito. Si no caben todas trunca
            // por grupos (los que no entran salen con instanceCount 0 y se
            // descartan) antes que escribir fuera de rango: el llamante
            // dimensiona el buffer antes, esto es la red de seguridad.
            static uint32_t buildInstanceBatches(const BatchCandidate* candidates,
                                                 size_t                count,
                                                 glm::mat4*            outTransforms,
                                                 uint32_t              outCapacity,
                                                 uint32_t              firstInstanceBase,
                                                 std::vector<InstanceBatch>& outBatches);

        private:

            // Una instancia dibujable. Ya NO posee recursos GPU: buffers,
            // texturas y descriptor set viven en la entrada compartida que
            // apunta sharedIndex, y N objetos con la misma malla+material
            // apuntan todos a la misma. Lo único por instancia es el transform
            // (y el nombre, que es de depuración).
            struct RenderObject
            {
                std::string     name;
                // -1 = sin recursos (nunca construido, o ya liberado desde el
                // editor). Es el chequeo que sustituye al viejo
                // "vertexBuffer == VK_NULL_HANDLE".
                int             sharedIndex         = -1;
                glm::mat4       transform{1.0f};
            };

            struct SkinnedMatGfx {
                VkImage         textureImage  = VK_NULL_HANDLE;
                VkDeviceMemory  textureMem    = VK_NULL_HANDLE;
                VkImageView     textureView   = VK_NULL_HANDLE;
                VkSampler       sampler       = VK_NULL_HANDLE;
                VkImage         normalImage   = VK_NULL_HANDLE;
                VkDeviceMemory  normalMem     = VK_NULL_HANDLE;
                VkImageView     normalView    = VK_NULL_HANDLE;
                VkSampler       normalSampler = VK_NULL_HANDLE;
                VkImage         ormImage      = VK_NULL_HANDLE;
                VkDeviceMemory  ormMem        = VK_NULL_HANDLE;
                VkImageView     ormView       = VK_NULL_HANDLE;
                VkSampler       ormSampler    = VK_NULL_HANDLE;
                float           metallic      = 0.0f;
                float           roughness     = 0.5f;
                VkDescriptorSet descSets[2]   = {};
            };

            struct SubMeshDraw {
                uint32_t indexStart;
                uint32_t indexCount;
                uint32_t materialIndex;
            };

            // ABI compartida por los 3 compute shaders. 16 bytes, fijos: el 4º
            // campo era un pad sin usar y ahora lleva el clipBase, así que
            // ningún offset se ha movido.
            struct ComputePush
            {
                float animTime;
                uint32_t boneCount;
                uint32_t vertexCount;
                // activeClip * boneCount: índice base del bloque del clip activo
                // dentro del SSBO de BoneInfos, que va en layout [clip][hueso].
                // Solo lo lee bone_eval.comp; bone_hierarchy y skinning declaran
                // este slot como "pad" y no lo tocan.
                uint32_t clipBase;
            };
            static_assert(sizeof(ComputePush) == 16, "ComputePush debe seguir en 16 bytes: los 3 .comp declaran este layout");

            struct PushData {
                glm::mat4 transform{1.0f};
                float     metallic  = 1.0f;
                float     roughness = 1.0f;
                // flags.x = 1: triangle.vert coge el model matrix del SSBO de
                // instancias por gl_InstanceIndex (ruta estática, agrupada);
                // 0: lo coge de `transform` (ruta skinned, que comparte este
                // vertex shader y dibuja una instancia con su propia matriz).
                // Es el viejo _pad reaprovechado: mismo tipo y offset, así que
                // pbr.frag sigue declarando el bloque igual que siempre.
                glm::vec2 flags{0.0f, 0.0f};
            };
            static_assert(sizeof(PushData) == 80, "PushData must be 80 bytes");

            struct SkinnedRenderObject {
                std::string    name;
                // SSBOs estáticos
                VkBuffer       keyframePosBuffer    = VK_NULL_HANDLE;
                VkDeviceMemory keyframePosMemory    = VK_NULL_HANDLE;
                VkBuffer       keyframeRotBuffer    = VK_NULL_HANDLE;
                VkDeviceMemory keyframeRotMemory    = VK_NULL_HANDLE;
                VkBuffer       keyframeScaleBuffer  = VK_NULL_HANDLE;
                VkDeviceMemory keyframeScaleMemory  = VK_NULL_HANDLE;
                VkBuffer       boneInfoBuffer       = VK_NULL_HANDLE;
                VkDeviceMemory boneInfoMemory       = VK_NULL_HANDLE;
                VkBuffer       inputVertexBuffer    = VK_NULL_HANDLE;
                VkDeviceMemory inputVertexMemory    = VK_NULL_HANDLE;
                // SSBOs dinámicos (escritos por compute)
                VkBuffer       localTransformBuffer = VK_NULL_HANDLE;
                VkDeviceMemory localTransformMemory = VK_NULL_HANDLE;
                VkBuffer       finalBoneBuffer      = VK_NULL_HANDLE;
                VkDeviceMemory finalBoneMemory      = VK_NULL_HANDLE;
                // Output vertex buffer (usado también como VB en graphics)
                VkBuffer       outputVertexBuffer   = VK_NULL_HANDLE;
                VkDeviceMemory outputVertexMemory   = VK_NULL_HANDLE;
                // Index buffer
                VkBuffer       indexBuffer          = VK_NULL_HANDLE;
                VkDeviceMemory indexMemory          = VK_NULL_HANDLE;
                uint32_t       indexCount           = 0;
                uint32_t       vertexCount          = 0;
                uint32_t       boneCount            = 0;
                // Nº de clips concatenados en los SSBOs de keyframes. Solo se usa
                // pa clampar en setAnimationState (Task 3): un clipIndex fuera de
                // rango haría que clipBase apuntara fuera del SSBO de BoneInfos y
                // el compute leyera basura sin que nada avisara.
                uint32_t       clipCount            = 1;
                // Descriptor set de compute
                VkDescriptorSet computeDescSet      = VK_NULL_HANDLE;
                // Texturas y descriptor sets por material
                std::vector<SkinnedMatGfx>  matGfx;
                std::vector<SubMeshDraw>    subMeshes;
                // Estado de animación
                float     animTime       = 0.0f;
                // Índice del clip que se evalúa este frame. Sin blending solo se
                // evalúa uno: los demás residen en el SSBO y no se leen.
                uint32_t  activeClip     = 0;
                float     duration       = 0.0f;
                float     ticksPerSecond = 24.0f;
                glm::mat4 transform      {1.0f};
                // Cota para el frustum culling: esfera centrada en el origen
                // local, válida en toda pose (ver skinnedBoundRadius).
                // hasBounds false = malla sin con qué acotar -> no se culea
                // nunca, que es el lado seguro.
                float     boundRadius    = 0.0f;
                bool      hasBounds      = false;
                // Lado mayor de la AABB de la pose de REPOSO, en espacio local.
                // Solo lo usa el grosor del contorno de selección, que es
                // proporcional al tamaño del objeto: boundRadius no vale ahí
                // porque acota todas las poses y sale varias veces mayor que la
                // malla. 0 = malla sin vértices (el contorno cae a su mínimo).
                float     restMaxExtent  = 0.0f;
                // 0 = subido y visible. >0 = esperando a que la fence del batch
                // con ese ticket señale. Sin esto, el objeto se dibujaría con
                // sus texturas todavía en TRANSFER_DST_OPTIMAL.
                uint64_t  uploadTicket   = 0;
            };

            void createSwapChain(Window& window);
            void createImageViews();
            void createOffscreenRenderPass();
            void createRenderPass();
            void createFramebuffers();
            void createOffscreenImages();
            void destroyOffscreenImages();
            void createCommandBuffers();
            void createSyncObjects();
            void recordCommandBuffer(uint32_t imageIndex);
            // Casco invertido del objeto seleccionado, al final del pass de
            // escena y antes del skybox. camFrustum es el mismo del culling del
            // frame: el objeto resaltado se vuelve a evaluar con el mismo
            // criterio, no se le da paso libre. No-op si no hay seleccion.
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
            void createDescriptorSets();
            void updateUniformBuffer(uint32_t frameIndex);
            void createDepthResources();
            // Resuelve mesh a una entrada compartida y deja obj apuntando a
            // ella: la crea (buffers + texturas) solo si ningún otro objeto
            // había subido ya esa misma malla+material. Devuelve true si ha
            // tenido que crearla — el caller lo usa pa saber si además hay que
            // alojarle el descriptor set.
            bool buildRenderObject(const Mesh& mesh, RenderObject& obj,
                                   TransferBatch* batch = nullptr,
                                   const std::vector<DecodedImage>* decoded = nullptr);
            // Rellena una entrada recién creada. Es el cuerpo que antes estaba
            // en buildRenderObject, sin la parte de resolución de la clave.
            void createSharedGpuMesh(const Mesh& mesh, SharedGpuMesh& gpu,
                                     TransferBatch* batch,
                                     const std::vector<DecodedImage>* decoded);
            void allocateObjectDescriptorSet(SharedGpuMesh& gpu);
            void destroySharedGpuMesh(const SharedGpuMesh& gpu);
            void createShadowResources();
            void recordShadowPass(VkCommandBuffer cmd);
            void createComputePipelines();
            void destroySkinnedRenderObject(SkinnedRenderObject& obj);
            // Cuerpo compartido por addSkinnedMesh y rebuildSkinnedMesh: crea
            // buffers, sube SSBOs, aloja descriptor sets y carga texturas sobre
            // un SkinnedRenderObject ya vacío.
            void initSkinnedRenderObject(SkinnedRenderObject& obj, const SkinnedMesh& mesh,
                                         TransferBatch* batch = nullptr,
                                         const std::vector<DecodedImage>* decoded = nullptr);
            void recordComputePass(VkCommandBuffer cmd);
            void removeStaticObject(int index);
            void removeSkinnedObject(int index);

            // Sueltan la referencia / encolan la destrucción en lugar de
            // ejecutarla. Son el ÚNICO camino permitido: llamar a
            // destroySharedGpuMesh directamente desde un call site nuevo
            // destruiría recursos que otros objetos siguen usando, y aunque no
            // los hubiera volvería a necesitar un vkDeviceWaitIdle que nadie se
            // acordaría de poner.
            //
            // releaseRenderObject deja obj.sharedIndex en -1 y solo encola la
            // destrucción cuando cae el último holder de la entrada.
            void releaseRenderObject(RenderObject& obj);
            void queueDestroySkinnedRenderObject(SkinnedRenderObject& obj);

            // Cámara efectiva de un frame. eye va aquí porque ubo.viewPos
            // alimenta el specular: sin él, en Play los brillos se calcularían
            // desde la posición de la cámara del editor.
            struct FrameCamera {
                glm::mat4 view;
                glm::mat4 proj;
                glm::vec3 eye;
            };

            // La del CameraComponent en Play (si la escena tiene una), la de
            // vuelo del editor en cualquier otro caso. Único sitio donde se
            // decide: antes la proyección estaba duplicada a pelo en
            // recordCommandBuffer y updateUniformBuffer.
            FrameCamera currentFrameCamera() const;

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

            // Offscreen render target (escena 3D → textura muestreada por la UI)
            VkRenderPass                    m_offscreenRenderPass               = VK_NULL_HANDLE;
            VkImage                         m_offscreenImage[MAX_FRAMES]        = {};
            VkDeviceMemory                  m_offscreenMemory[MAX_FRAMES]       = {};
            VkImageView                     m_offscreenView[MAX_FRAMES]         = {};
            VkSampler                       m_offscreenSampler                  = VK_NULL_HANDLE;
            VkFramebuffer                   m_offscreenFramebuffer[MAX_FRAMES]  = {};
            VkDescriptorSet                 m_offscreenDescSet[MAX_FRAMES]      = {};

            VkSemaphore                     m_imageAvailable[MAX_FRAMES]        = {};
            std::vector<VkSemaphore>        m_renderFinished;
            VkFence                         m_inFlight[MAX_FRAMES]              = {};
            int                             m_currentFrame                      = 0;
            VkPipelineLayout                m_pipelineLayout                    = VK_NULL_HANDLE;
            VkPipeline                      m_pipeline                          = VK_NULL_HANDLE;
            VkPipeline                      m_wireframePipeline                 = VK_NULL_HANDLE;
            // Casco invertido del objeto seleccionado. Dos pipelines por el
            // mismo motivo que los dos wireframe: el vertex input estatico
            // (Vertex) y el skinned (OutputVertex) tienen stride distinto.
            VkPipeline                      m_outlinePipeline                   = VK_NULL_HANDLE;
            VkPipeline                      m_skinnedOutlinePipeline            = VK_NULL_HANDLE;
            // Variante LINE de los dos anteriores, para cuando la escena se
            // dibuja en wireframe: ahí el interior del objeto no escribe
            // profundidad (solo lo hacen las aristas rasterizadas), así que un
            // casco relleno pasaría el depth test entero y taparía el objeto de
            // color plano en vez de bordearlo.
            VkPipeline                      m_outlineWirePipeline               = VK_NULL_HANDLE;
            VkPipeline                      m_skinnedOutlineWirePipeline        = VK_NULL_HANDLE;
            // Objeto resaltado, -1 = ninguno. Solo los fija el editor
            // (setOutlineTarget); en runtime se quedan en -1 para siempre.
            int                             m_outlineStaticIndex                = -1;
            int                             m_outlineSkinnedIndex               = -1;
            bool                            m_framebufferResized                = false;
            bool                            m_wireframeMode                     = false;
            bool                            m_headless                          = false;
            VkDescriptorSetLayout           m_descriptorSetLayout               = VK_NULL_HANDLE;
            VkBuffer                        m_uniformBuffers[MAX_FRAMES]        = {};
            VkDeviceMemory                  m_uniformBuffersMemory[MAX_FRAMES]  = {};
            void*                           m_uniformBuffersMapped[MAX_FRAMES]  = {};
            VkDescriptorPool                m_descriptorPool                    = VK_NULL_HANDLE;
            // ── SSBO de transforms por instancia (set 1, binding 0) ──────────
            // Uno por frame-in-flight y mapeado en persistente: el frame
            // anterior puede seguir en vuelo leyendo el suyo. Los dos passes
            // (sombras primero, escena después) comparten el buffer del frame:
            // m_instanceCursor es el nº de matrices ya escritas y hace de base
            // del siguiente pass.
            VkDescriptorSetLayout           m_instanceDescLayout                = VK_NULL_HANDLE;
            VkDescriptorPool                m_instanceDescPool                  = VK_NULL_HANDLE;
            VkDescriptorSet                 m_instanceDescSets[MAX_FRAMES]      = {};
            VkBuffer                        m_instanceBuffers[MAX_FRAMES]       = {};
            VkDeviceMemory                  m_instanceMemory[MAX_FRAMES]        = {};
            void*                           m_instanceMapped[MAX_FRAMES]        = {};
            uint32_t                        m_instanceCapacity[MAX_FRAMES]      = {}; // en matrices
            uint32_t                        m_instanceCursor                    = 0;
            // Scratch reutilizado entre frames y entre passes: el agrupado corre
            // dos veces por frame y no debe alojar nada en ese camino.
            std::vector<BatchCandidate>     m_batchCandidates;
            std::vector<InstanceBatch>      m_instanceBatches;
            // Visibilidad de m_skinnedObjects de ESTE frame, en el mismo orden e
            // indexada igual. Se calcula una sola vez al principio de
            // recordCommandBuffer porque la leen dos sitios: el compute (que va
            // primero en el command buffer) y el dibujo. Que compartan la misma
            // decisión es lo que evita el pop: si el compute se saltara un
            // objeto que luego SÍ se dibuja, su outputVertexBuffer conservaría
            // la pose del último frame en que fue visible.
            std::vector<uint8_t>            m_skinnedVisible;
            void createInstanceResources();
            // Asegura sitio para `matrices` en el buffer del frame actual. Se
            // llama al principio de recordCommandBuffer, con la fence del frame
            // ya esperada: recrear el buffer aquí no pisa nada en vuelo.
            void ensureInstanceCapacity(uint32_t matrices);
            void destroyInstanceBuffer(int frame);
            VkImage                         m_depthImage                        = VK_NULL_HANDLE;
            VkDeviceMemory                  m_depthImageMemory                  = VK_NULL_HANDLE;
            VkImageView                     m_depthImageView                    = VK_NULL_HANDLE;
            glm::vec3                       m_cameraTarget{0.0f};
            float                           m_cameraDistance{5.0f};
            glm::mat4                       m_viewMatrix{1.0f};
            Camera                          m_camera;
            std::vector<Light>              m_lights;
            
            // Shadow Map (cascadas)
            static constexpr uint32_t       SHADOW_SIZE                         = 2048;
            VkImage                         m_shadowImage                       = VK_NULL_HANDLE;
            VkDeviceMemory                  m_shadowMemory                      = VK_NULL_HANDLE;
            // Vista del array completo: es la que muestrea el fragment shader
            // (sampler2DArrayShadow) y la que va en los descriptor sets.
            VkImageView                     m_shadowView                        = VK_NULL_HANDLE;
            // Una vista de UNA capa por cascada. Solo existen para poder colgar
            // un framebuffer de cada capa; nadie las muestrea.
            VkImageView                     m_shadowLayerViews[SHADOW_CASCADES] {};
            VkSampler                       m_shadowSampler                     = VK_NULL_HANDLE;
            VkRenderPass                    m_shadowRenderPass                  = VK_NULL_HANDLE;
            VkFramebuffer                   m_shadowFramebuffers[SHADOW_CASCADES] {};
            VkPipeline                      m_shadowPipeline                    = VK_NULL_HANDLE;
            VkPipelineLayout                m_shadowPipelineLayout              = VK_NULL_HANDLE;
            // Caché por frame que rellena computeCascades(). Identidad y 0 si la
            // escena no tiene luces: en ese caso el pass solo limpia las capas.
            glm::mat4                       m_cascadeMatrices[SHADOW_CASCADES]  { glm::mat4(1.0f), glm::mat4(1.0f), glm::mat4(1.0f), glm::mat4(1.0f) };
            glm::vec4                       m_cascadeSplits                     { 0.0f };
            // Compute pipelines
            VkPipeline            m_boneEvalPipeline      = VK_NULL_HANDLE;
            VkPipeline            m_boneHierarchyPipeline = VK_NULL_HANDLE;
            VkPipeline            m_skinningPipeline      = VK_NULL_HANDLE;
            VkPipeline            m_skinnedGfxPipeline        = VK_NULL_HANDLE;
            VkPipeline            m_skinnedWireframePipeline  = VK_NULL_HANDLE;
            VkPipelineLayout      m_computePipelineLayout = VK_NULL_HANDLE;
            VkDescriptorSetLayout m_computeDescLayout     = VK_NULL_HANDLE;
            VkDescriptorPool      m_computeDescPool       = VK_NULL_HANDLE;
            std::vector<SkinnedRenderObject> m_skinnedObjects;

            std::vector<RenderObject> m_objects;

            // Recursos GPU compartidos por los objetos estáticos. Los objetos
            // guardan un índice aquí; la tabla los mantiene vivos mientras
            // quede algún holder. (Los skinned no comparten: sus SSBOs de
            // salida los escribe el compute por instancia.)
            SharedGpuMeshCache m_sharedMeshes;

            // Batch abierto donde caen los uploads del pump actual. Se envía en
            // flushPendingUploads() y pasa a m_inFlightBatches.
            std::unique_ptr<TransferBatch> m_pendingBatch;
            struct InFlightBatch { uint64_t ticket; std::unique_ptr<TransferBatch> batch; };
            std::vector<InFlightBatch>     m_inFlightBatches;
            uint64_t                       m_nextUploadTicket      = 1;
            uint64_t                       m_lastCompletedTicket   = 0;

            DeferredDeleteQueue m_deferredDeletes;

            UiLayer* m_ui = nullptr;
            Skybox   m_skybox;
            SplashScreen m_splash;
            GameObject* m_sceneRoot = nullptr;
            Scene* m_scene = nullptr;
    };
}