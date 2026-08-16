#pragma once
#include <vulkan/vulkan.h>
#include <glm/glm.hpp>
#include <vector>
#include <unordered_map>
#include <cstdint>
#include "DonTopo/Renderer/Passes/IblPass.h"
#include "DonTopo/Renderer/Passes/ForwardPlusPass.h"
#include "DonTopo/Renderer/RenderObjects.h"
#include "DonTopo/Renderer/SharedGpuMesh.h"

namespace DonTopo {

class GpuDevice;
class Scene;
class Skybox;

// Sondas de entorno: capturan la escena desde su posicion en 6 caras y
// sustituyen al IBL global (bindings 5 y 6 del set 0) en los objetos que caen
// dentro de su radio. El bake es un EVENTO: no graba ni un comando en el
// command buffer del frame, asi que el coste GPU por frame con N sondas ya
// bakeadas es exactamente el mismo que con 0.
//
// Lado de captura: un solo cubemap COMPARTIDO por todas las sondas (es un
// intermedio del bake, no persiste), creado la primera vez que hay algo que
// bakear. Sin sondas no se crea y no gasta nada.
//
// Ataduras con codigo que no es suyo, todas por el Context:
//  - el bake REDIBUJA la escena, asi que necesita el pass offscreen entero del
//    Renderer (render pass, framebuffer y HDR del slot 0, los dos pipelines de
//    escena con su layout, el set de instancias y el UBO mapeado) mas las
//    listas de objetos. Nada de eso se mueve: viaja por referencia.
//  - los pipelines de convolucion y las dos vistas globales son de IblPass.
//  - fp: el bake tiene que APAGAR el Forward+ mientras captura (su rejilla se
//    culleo contra el frustum de la camara del frame, no contra las 6 caras).
//    Es la unica atadura que no cabe en un handle, porque muta estado del otro
//    pase; sigue siendo pase->pase y nunca pase->Renderer.
class ReflectionProbePass {
public:
    static constexpr uint32_t kFaceSize = 128;
    // 7 pares: uno por cara mas el de la convolucion. Se suman los deltas en
    // vez de medir del primero al ultimo, que contaria tambien las esperas del
    // host entre submits.
    static constexpr uint32_t kQueryCount = 14;

    struct GpuProbe
    {
        uint64_t  ownerId  = 0;          // GameObject::id de la sonda
        glm::vec3 position { 0.0f };
        float     radius    = 0.0f;
        float     intensity = 1.0f;
        // Mismas dos imagenes que el IBL global, por sonda: irradiancia
        // (1 mip) y entorno prefiltrado (IblPass::kPrefilterMips).
        VkImage        irradianceImage  = VK_NULL_HANDLE;
        VkDeviceMemory irradianceMemory = VK_NULL_HANDLE;
        VkImageView    irradianceView   = VK_NULL_HANDLE;
        VkImageView    irradianceStore  = VK_NULL_HANDLE;
        VkImage        prefilterImage   = VK_NULL_HANDLE;
        VkDeviceMemory prefilterMemory  = VK_NULL_HANDLE;
        VkImageView    prefilterView    = VK_NULL_HANDLE;
        VkImageView    prefilterStore[IblPass::kPrefilterMips] {};
        bool           baked  = false;   // false: todavia con el neutro
        float          bakeMs = 0.0f;    // ultimo bake, timestamps GPU
        // Llamadas a sync() seguidas SIN cambios en los ajustes de la sonda. El
        // auto-bake espera a que llegue a 1: sin esto, arrastrar el slider de
        // Intensity dispararia un bake por frame (con su vkDeviceWaitIdle y sus
        // 7 submits).
        int            settleFrames = 0;
    };

    struct Context {
        GpuDevice& gpu;
        // nullptr = sin escena cargada: no hay arbol que recorrer.
        Scene*     scene;
        // El cielo se dibuja en cada una de las seis caras.
        Skybox&    skybox;

        // ── IBL global (handles de IblPass) ──────────────────────────────────
        VkPipeline            iblIrradiancePipeline;
        VkPipeline            iblPrefilterPipeline;
        VkPipelineLayout      iblPipelineLayout;
        VkDescriptorPool      iblDescPool;
        VkDescriptorSetLayout iblDescLayout;
        VkSampler             iblSampler;
        VkImageView           globalIrradianceView;
        VkImageView           globalPrefilterView;

        // ── El pass de escena, tal cual lo graba el frame ────────────────────
        // Resolucion INTERNA del render, no la del swapchain: la cara se
        // recorta a min(kFaceSize, extent) porque el framebuffer es el del
        // viewport.
        const VkExtent2D& renderExtent;
        VkRenderPass      sceneRenderPass;
        VkFramebuffer     sceneFramebuffer;    // el del slot 0
        VkImage           hdrImage;            // idem, origen del blit
        VkPipeline        scenePipeline;
        VkPipeline        skinnedPipeline;
        VkPipelineLayout  scenePipelineLayout;
        VkDescriptorSet   instanceSet;         // set 1, slot 0
        void*             uboMapped;           // UBO del slot 0, mapeado
        // false = todavia no se ha escrito un frame: el UBO del slot 0 es
        // basura (ni luces ni matrices de cascada) y los bakes esperan.
        bool              uboWritten;
        ForwardPlusPass&  fp;

        // ── Lo que hay que dibujar ───────────────────────────────────────────
        const std::vector<RenderObject>&  objects;
        SharedGpuMeshCache&               sharedMeshes;
        std::vector<SkinnedRenderObject>& skinnedObjects;
        uint64_t                          lastCompletedTicket;

        // El mapa de AO del slot 0: se limpia a 1.0 antes de capturar, porque
        // el que hay en la GPU es el de la camara del frame. VK_NULL_HANDLE si
        // el SSAO nunca ha creado sus imagenes.
        VkImage ssaoBlurImage;

        bool  timestampsSupported;
        float timestampPeriod;
    };

    ReflectionProbePass()                                      = default;
    ReflectionProbePass(const ReflectionProbePass&)            = delete;
    ReflectionProbePass& operator=(const ReflectionProbePass&) = delete;

    // Reconcilia la lista de sondas con la escena, lanza los bakes pendientes y
    // reasigna sonda->objeto. Una vez por frame, al principio de drawFrame: es
    // donde se puede esperar a que la GPU quede libre sin pillar el command
    // buffer a medio grabar.
    void sync(const Context& ctx);
    void destroy(const Context& ctx);

    // La UI solo ENCOLA: el bake ocurre en el sync del frame siguiente.
    void requestBake(uint64_t ownerId) { m_bakeQueue.push_back(ownerId); }
    void requestBakeAll()              { m_bakeAllQueued = true; }

    int   count()      const { return (int)m_probes.size(); }
    // ms del ULTIMO bake (una sonda o la tanda entera), por timestamps.
    float lastBakeMs() const { return m_lastBakeMs; }
    // ms del ultimo bake de UNA sonda concreta, o -1 si nunca se bakeo.
    float bakeMs(uint64_t ownerId) const
    {
        for (const GpuProbe& p : m_probes)
            if (p.ownerId == ownerId) return p.baked ? p.bakeMs : -1.0f;
        return -1.0f;
    }

    // Memoria GPU de las capturas persistentes de UNA sonda, en bytes.
    // No cuenta el cubemap de captura, que es uno solo pa todas.
    static constexpr uint64_t probeMemoryBytes()
    {
        // rgba16f = 8 bytes/texel, 6 caras. El prefiltrado suma sus mips
        // (la serie 1 + 1/4 + 1/16 + ... truncada a kPrefilterMips).
        uint64_t pre = 0;
        for (uint32_t m = 0; m < IblPass::kPrefilterMips; m++)
        {
            const uint64_t s = IblPass::kPrefilterSize >> m;
            pre += s * s * 6ull * 8ull;
        }
        return (uint64_t)IblPass::kIrradianceSize * IblPass::kIrradianceSize * 6ull * 8ull + pre;
    }

private:
    // Cubemap intermedio del bake y su query pool. La primera vez que hay algo
    // que bakear.
    void createCapture(const Context& ctx);
    void createProbeImages(const Context& ctx, GpuProbe& probe);
    void destroyProbeImages(const Context& ctx, GpuProbe& probe);
    // Las 6 caras + la convolucion de UNA sonda. Submits propios, no toca el
    // command buffer del frame.
    void bake(const Context& ctx, GpuProbe& probe);
    // La sonda MAS CERCANA cuyo radio contiene el punto. -1 = ninguna.
    int  pickProbeFor(const glm::vec3& worldPos) const;
    // Calcula la asignacion DESEADA y solo toca la GPU si difiere de la ya
    // escrita.
    void refreshAssignment(const Context& ctx);
    // Devuelve TODOS los objetos al IBL global. Justo antes de una tanda de
    // bakes, o la captura se realimenta.
    void assignAllToGlobalIbl(const Context& ctx);
    // Los bindings 5 y 6 de un set ya alojado, con el sampler del IBL.
    void writeIblBindings(const Context& ctx, VkDescriptorSet set,
                          VkImageView irradiance, VkImageView prefilter) const;

    std::vector<GpuProbe> m_probes;
    VkImage        m_captureImage  = VK_NULL_HANDLE;
    VkDeviceMemory m_captureMemory = VK_NULL_HANDLE;
    VkImageView    m_captureView   = VK_NULL_HANDLE;
    VkQueryPool    m_queryPool     = VK_NULL_HANDLE;

    std::vector<uint64_t> m_bakeQueue;
    bool                  m_bakeAllQueued = false;
    float                 m_lastBakeMs    = 0.0f;

    // Asignacion resuelta: sharedIndex -> indice en m_probes (-1 = IBL
    // global). Es la CACHE de lo ya escrito en los descriptor sets; solo se
    // reescriben bindings cuando el mapa recalculado difiere de este.
    std::unordered_map<int, int> m_assignShared;
    std::vector<int>             m_assignSkinned;
};

} // namespace DonTopo
