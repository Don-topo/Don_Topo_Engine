#include "DonTopo/Renderer/Passes/ReflectionProbePass.h"
#include "DonTopo/Renderer/GpuDevice.h"
#include "DonTopo/Renderer/Skybox.h"
#include "DonTopo/Renderer/UniformBufferObject.h"
#include "DonTopo/Core/Scene.h"
#include "DonTopo/Core/GameObject.h"
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <stdexcept>
#include <vector>
#include "DonTopo/Renderer/Passes/IblPass.h"
#include "DonTopo/Core/ReflectionProbeComponent.h"

namespace DonTopo {

// Frames en vuelo del Renderer: es el tamaño de los arrays de descriptor sets
// de SharedGpuMesh y de SkinnedMatGfx, que este pase reescribe.
static constexpr int kFrames = 2;
static_assert(sizeof(SharedGpuMesh::descriptorSets) / sizeof(VkDescriptorSet) == kFrames,
              "kFrames debe seguir el numero de descriptor sets por malla compartida");

// ── Reflection probes ───────────────────────────────────────────────────────
// Nada de lo que hay aqui graba un solo comando en el command buffer del
// frame: el bake son submits propios, disparados por un evento. Con las sondas
// ya bakeadas el frame cuesta exactamente lo mismo que con ninguna, porque lo
// unico que cambia son DOS descriptores (bindings 5 y 6 del set 0) que ya
// estaban ahi apuntando al IBL global.

void ReflectionProbePass::createCapture(const Context& ctx)
{
    // Cubemap intermedio del bake, UNO solo pa todas las sondas: solo tiene
    // que vivir entre el render de las 6 caras y la convolucion. Se crea la
    // primera vez que hay algo que bakear, asi que una escena sin sondas no
    // gasta ni un byte por esta feature.
    VkImageCreateInfo ci{};
    ci.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ci.flags         = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    ci.imageType     = VK_IMAGE_TYPE_2D;
    ci.format        = IblPass::kFormat;
    ci.extent        = { kFaceSize, kFaceSize, 1 };
    ci.mipLevels     = 1;
    ci.arrayLayers   = 6;
    ci.samples       = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling        = VK_IMAGE_TILING_OPTIMAL;
    // TRANSFER_DST: destino del blit desde el HDR de la escena, una cara por submit.
    ci.usage         = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ci.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(ctx.gpu.device(), &ci, nullptr, &m_captureImage) != VK_SUCCESS)
        throw std::runtime_error("failed to create probe capture cubemap!");

    VkMemoryRequirements memReq;
    vkGetImageMemoryRequirements(ctx.gpu.device(), m_captureImage, &memReq);
    VkMemoryAllocateInfo memAlloc{};
    memAlloc.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    memAlloc.allocationSize  = memReq.size;
    memAlloc.memoryTypeIndex = ctx.gpu.findMemoryType(memReq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(ctx.gpu.device(), &memAlloc, nullptr, &m_captureMemory) != VK_SUCCESS)
        throw std::runtime_error("failed to allocate probe capture memory!");
    vkBindImageMemory(ctx.gpu.device(), m_captureImage, m_captureMemory, 0);

    VkImageViewCreateInfo vi{};
    vi.sType                           = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image                           = m_captureImage;
    vi.viewType                        = VK_IMAGE_VIEW_TYPE_CUBE;
    vi.format                          = IblPass::kFormat;
    vi.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    vi.subresourceRange.baseMipLevel   = 0;
    vi.subresourceRange.levelCount     = 1;
    vi.subresourceRange.baseArrayLayer = 0;
    vi.subresourceRange.layerCount     = 6;
    if (vkCreateImageView(ctx.gpu.device(), &vi, nullptr, &m_captureView) != VK_SUCCESS)
        throw std::runtime_error("failed to create probe capture view!");

    // Arranca en SHADER_READ_ONLY, que es el layout desde el que el bake la
    // mueve a TRANSFER_DST y al que la devuelve. El contenido inicial da
    // igual: el bake escribe las 6 caras antes de que nadie las lea.
    {
        VkCommandBuffer cmd = ctx.gpu.beginOneTimeCommands();
        VkImageMemoryBarrier b{};
        b.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image               = m_captureImage;
        b.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        b.srcAccessMask       = 0;
        b.dstAccessMask       = VK_ACCESS_SHADER_READ_BIT;
        b.subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 6 };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &b);
        ctx.gpu.endOneTimeCommands(cmd);
    }

    // Query pool propio del bake: 7 pares (6 caras + convolucion). No se
    // mezcla con el del AA ni con el del bloom, que se resetean por frame.
    if (ctx.timestampsSupported && m_queryPool == VK_NULL_HANDLE)
    {
        VkQueryPoolCreateInfo qi{};
        qi.sType      = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        qi.queryType  = VK_QUERY_TYPE_TIMESTAMP;
        qi.queryCount = kQueryCount;
        if (vkCreateQueryPool(ctx.gpu.device(), &qi, nullptr, &m_queryPool) != VK_SUCCESS)
            throw std::runtime_error("failed to create probe query pool!");
    }
}

void ReflectionProbePass::createProbeImages(const Context& ctx, GpuProbe& probe)
{
    // Mismas dos imagenes que el IBL global (IblPass::createResources), pero
    // por sonda. m_res.createImage no vale: fija arrayLayers y mipLevels a 1.
    auto makeCube = [&](uint32_t size, uint32_t mips, VkImage& image, VkDeviceMemory& memory)
    {
        VkImageCreateInfo ci{};
        ci.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ci.flags         = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
        ci.imageType     = VK_IMAGE_TYPE_2D;
        ci.format        = IblPass::kFormat;
        ci.extent        = { size, size, 1 };
        ci.mipLevels     = mips;
        ci.arrayLayers   = 6;
        ci.samples       = VK_SAMPLE_COUNT_1_BIT;
        ci.tiling        = VK_IMAGE_TILING_OPTIMAL;
        ci.usage         = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT
                         | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        ci.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
        ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkCreateImage(ctx.gpu.device(), &ci, nullptr, &image) != VK_SUCCESS)
            throw std::runtime_error("failed to create probe cubemap image!");

        VkMemoryRequirements memReq;
        vkGetImageMemoryRequirements(ctx.gpu.device(), image, &memReq);
        VkMemoryAllocateInfo memAlloc{};
        memAlloc.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        memAlloc.allocationSize  = memReq.size;
        memAlloc.memoryTypeIndex = ctx.gpu.findMemoryType(memReq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (vkAllocateMemory(ctx.gpu.device(), &memAlloc, nullptr, &memory) != VK_SUCCESS)
            throw std::runtime_error("failed to allocate probe cubemap memory!");
        vkBindImageMemory(ctx.gpu.device(), image, memory, 0);
    };

    makeCube(IblPass::kIrradianceSize, 1,                      probe.irradianceImage, probe.irradianceMemory);
    makeCube(IblPass::kPrefilterSize,  IblPass::kPrefilterMips, probe.prefilterImage,  probe.prefilterMemory);

    auto makeView = [&](VkImage image, VkImageViewType type, uint32_t baseMip, uint32_t mipCount, VkImageView& view)
    {
        VkImageViewCreateInfo vi{};
        vi.sType                           = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vi.image                           = image;
        vi.viewType                        = type;
        vi.format                          = IblPass::kFormat;
        vi.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        vi.subresourceRange.baseMipLevel   = baseMip;
        vi.subresourceRange.levelCount     = mipCount;
        vi.subresourceRange.baseArrayLayer = 0;
        vi.subresourceRange.layerCount     = 6;
        if (vkCreateImageView(ctx.gpu.device(), &vi, nullptr, &view) != VK_SUCCESS)
            throw std::runtime_error("failed to create probe image view!");
    };

    makeView(probe.irradianceImage, VK_IMAGE_VIEW_TYPE_CUBE,     0, 1, probe.irradianceView);
    makeView(probe.irradianceImage, VK_IMAGE_VIEW_TYPE_2D_ARRAY, 0, 1, probe.irradianceStore);
    makeView(probe.prefilterImage,  VK_IMAGE_VIEW_TYPE_CUBE,     0, IblPass::kPrefilterMips, probe.prefilterView);
    for (uint32_t m = 0; m < IblPass::kPrefilterMips; m++)
        makeView(probe.prefilterImage, VK_IMAGE_VIEW_TYPE_2D_ARRAY, m, 1, probe.prefilterStore[m]);

    // Contenido neutro, por el mismo motivo que en IblPass: entre que la sonda
    // existe y que alguien pulsa Bake, sus vistas ya estan en descriptor sets y
    // no pueden apuntar a memoria sin definir. Los mismos valores que el IBL
    // neutro, asi que una sonda recien creada y sin bakear se ve igual que el
    // ambiente plano de siempre.
    {
        VkCommandBuffer cmd = ctx.gpu.beginOneTimeCommands();

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
        clearTo(probe.irradianceImage, 1,                       irradianceNeutral);
        clearTo(probe.prefilterImage,  IblPass::kPrefilterMips, prefilterNeutral);

        ctx.gpu.endOneTimeCommands(cmd);
    }
}

void ReflectionProbePass::destroyProbeImages(const Context& ctx, GpuProbe& probe)
{
    // El caller ya ha esperado a que la GPU quede libre y ha reescrito los
    // bindings 5/6 que apuntaban aqui: al llegar a esta funcion ningun
    // descriptor set referencia estas vistas.
    vkDestroyImageView(ctx.gpu.device(), probe.irradianceView,  nullptr);
    vkDestroyImageView(ctx.gpu.device(), probe.irradianceStore, nullptr);
    vkDestroyImage(ctx.gpu.device(), probe.irradianceImage, nullptr);
    vkFreeMemory(ctx.gpu.device(), probe.irradianceMemory, nullptr);
    vkDestroyImageView(ctx.gpu.device(), probe.prefilterView, nullptr);
    for (uint32_t m = 0; m < IblPass::kPrefilterMips; m++)
        vkDestroyImageView(ctx.gpu.device(), probe.prefilterStore[m], nullptr);
    vkDestroyImage(ctx.gpu.device(), probe.prefilterImage, nullptr);
    vkFreeMemory(ctx.gpu.device(), probe.prefilterMemory, nullptr);
    probe = GpuProbe{};
}

void ReflectionProbePass::destroy(const Context& ctx)
{
    // El cubemap de captura y el query pool solo existen si alguna vez se
    // bakeo algo; las sondas, si la escena tenia alguna.
    for (GpuProbe& probe : m_probes) destroyProbeImages(ctx, probe);
    m_probes.clear();
    if (m_captureView != VK_NULL_HANDLE)
    {
        vkDestroyImageView(ctx.gpu.device(), m_captureView, nullptr);
        vkDestroyImage(ctx.gpu.device(), m_captureImage, nullptr);
        vkFreeMemory(ctx.gpu.device(), m_captureMemory, nullptr);
        m_captureView = VK_NULL_HANDLE;
    }
    if (m_queryPool != VK_NULL_HANDLE)
    {
        vkDestroyQueryPool(ctx.gpu.device(), m_queryPool, nullptr);
        m_queryPool = VK_NULL_HANDLE;
    }
}

void ReflectionProbePass::writeIblBindings(const Context& ctx, VkDescriptorSet set,
                                           VkImageView irradiance, VkImageView prefilter) const
{
    // Un write suelto sobre un set YA alojado, igual que writeSsaoBinding:
    // reescribir los bindings del IBL es lo unico que hace falta para que un
    // objeto pase del cubemap global al de una sonda. Ni layout nuevo, ni
    // miembro nuevo en el UBO, ni un indice en PushData (que esta a 80 bytes
    // justos).
    //
    // Los writes salen de IblPass, que es el dueño de esos dos bindings. Esta
    // era la CUARTA copia del mismo bloque —con las mallas, los personajes y el
    // propio IblPass—, y divergir aqui es de lo que no avisa nadie: el objeto
    // muestrearia el ambiente de otro.
    VkDescriptorImageInfo infos[2]{};
    VkWriteDescriptorSet  w[2]{};
    IblPass::fillIblWrites(set, irradiance, prefilter, ctx.iblSampler, infos, w);
    vkUpdateDescriptorSets(ctx.gpu.device(), 2, w, 0, nullptr);
}

int ReflectionProbePass::pickProbeFor(const glm::vec3& worldPos) const
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

void ReflectionProbePass::refreshAssignment(const Context& ctx)
{
    // Calcula la asignacion DESEADA y solo toca la GPU si difiere de la ya
    // escrita. En regimen estacionario esto son unas cuantas restas de
    // vectores en CPU y cero trabajo de GPU: ni un comando, ni un write.
    std::unordered_map<int, int> wantShared;
    for (const auto& obj : ctx.objects)
    {
        const SharedGpuMesh* gpu = ctx.sharedMeshes.get(obj.sharedIndex);
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

    std::vector<int> wantSkinned(ctx.skinnedObjects.size(), -1);
    for (size_t i = 0; i < ctx.skinnedObjects.size(); i++)
        wantSkinned[i] = pickProbeFor(glm::vec3(ctx.skinnedObjects[i].transform[3]));

    if (wantShared == m_assignShared && wantSkinned == m_assignSkinned) return;

    // Hay cambios: los sets pueden estar en uso por frames en vuelo.
    vkDeviceWaitIdle(ctx.gpu.device());

    auto viewsFor = [&](int probeIndex, VkImageView& irr, VkImageView& pre)
    {
        if (probeIndex < 0 || probeIndex >= (int)m_probes.size())
        {
            irr = ctx.globalIrradianceView;
            pre = ctx.globalPrefilterView;
        }
        else
        {
            irr = m_probes[probeIndex].irradianceView;
            pre = m_probes[probeIndex].prefilterView;
        }
    };

    for (const auto& entry : wantShared)
    {
        auto prev = m_assignShared.find(entry.first);
        if (prev != m_assignShared.end() && prev->second == entry.second) continue;
        SharedGpuMesh* gpu = ctx.sharedMeshes.get(entry.first);
        if (!gpu) continue;
        VkImageView irr, pre;
        viewsFor(entry.second, irr, pre);
        for (int i = 0; i < kFrames; i++)
            if (gpu->descriptorSets[i]) writeIblBindings(ctx, gpu->descriptorSets[i], irr, pre);
    }
    // Mallas que YA NO estan en el mapa deseado (objeto borrado) no hace
    // falta devolverlas al IBL global: sus sets se liberan con la malla.

    for (size_t si = 0; si < ctx.skinnedObjects.size(); si++)
    {
        const int want = wantSkinned[si];
        if (si < m_assignSkinned.size() && m_assignSkinned[si] == want) continue;
        VkImageView irr, pre;
        viewsFor(want, irr, pre);
        for (const SkinnedMatGfx& mgfx : ctx.skinnedObjects[si].matGfx)
            for (int i = 0; i < kFrames; i++)
                if (mgfx.descSets[i]) writeIblBindings(ctx, mgfx.descSets[i], irr, pre);
    }

    m_assignShared  = std::move(wantShared);
    m_assignSkinned = std::move(wantSkinned);
}

void ReflectionProbePass::assignAllToGlobalIbl(const Context& ctx)
{
    // Devuelve TODOS los objetos al IBL global. Se llama justo antes de una
    // tanda de bakes y no es un detalle: la captura reusa el pass de escena,
    // que ilumina cada objeto con lo que tenga en sus bindings 5/6. Si eso
    // es el cubemap de la propia sonda, cada bake vuelve a capturar la luz
    // que ya llevaba la intensidad aplicada y el efecto se amplifica bake a
    // bake (o se apaga, con intensidades bajas). Capturando siempre con el
    // IBL global el bake es idempotente y no depende del orden de las sondas.
    if (m_assignShared.empty() && m_assignSkinned.empty()) return;

    vkDeviceWaitIdle(ctx.gpu.device());
    for (int index : ctx.sharedMeshes.liveIndices())
    {
        SharedGpuMesh* gpu = ctx.sharedMeshes.get(index);
        for (int i = 0; i < kFrames; i++)
            if (gpu->descriptorSets[i])
                writeIblBindings(ctx, gpu->descriptorSets[i],
                                 ctx.globalIrradianceView, ctx.globalPrefilterView);
    }
    for (const SkinnedRenderObject& sobj : ctx.skinnedObjects)
        for (const SkinnedMatGfx& mgfx : sobj.matGfx)
            for (int i = 0; i < kFrames; i++)
                if (mgfx.descSets[i])
                    writeIblBindings(ctx, mgfx.descSets[i],
                                     ctx.globalIrradianceView, ctx.globalPrefilterView);

    // Las caches quedan vacias a proposito: refreshAssignment, al final de
    // sync(), vuelve a escribir la asignacion real.
    m_assignShared.clear();
    m_assignSkinned.clear();
}

void ReflectionProbePass::bake(const Context& ctx, GpuProbe& probe)
{
    // Sin framebuffer de escena (init temprano) o sin el SSBO de instancias
    // no hay contra que dibujar: la peticion se reintenta en otro frame.
    if (ctx.sceneFramebuffer == VK_NULL_HANDLE) return;
    if (ctx.instanceSet      == VK_NULL_HANDLE) return;

    if (m_captureImage == VK_NULL_HANDLE) createCapture(ctx);

    // Las 6 caras dibujan sobre el HDR del slot 0 y leen el UBO del frame 0,
    // que pueden estar en vuelo. Esto es un evento, no un pass: se puede esperar.
    vkDeviceWaitIdle(ctx.gpu.device());

    const uint32_t faceRender = std::min(kFaceSize,
                                         std::min(ctx.renderExtent.width, ctx.renderExtent.height));
    if (faceRender == 0) return;

    // Base: el UBO del frame 0 TAL CUAL. Luces, matrices de cascada y splits
    // se conservan a proposito — el shadow map que hay en la GPU es el de
    // esas matrices, y recomputarlas aqui lo descuadraria.
    UniformBufferObject ubo{};
    memcpy(&ubo, ctx.uboMapped, sizeof(ubo));
    ubo.viewPos = glm::vec4(probe.position, 1.0f);

    // Forward+ a Off durante la captura: su rejilla se culleo contra el
    // frustum de la camara del frame, no contra estas 6 caras. mode 0 es el
    // bucle clasico sobre las luces del UBO, con todas ellas. Se restaura al
    // salir; el modo que la UI tiene pedido no se toca.
    ForwardPlusPass::ParamsGpu savedFp{};
    const bool restoreFp = ctx.fp.overrideModeOff(savedFp);

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
        memcpy(ctx.uboMapped, &ubo, sizeof(ubo));

        VkCommandBuffer cmd = ctx.gpu.beginOneTimeCommands();

        if (ctx.timestampsSupported && m_queryPool != VK_NULL_HANDLE)
        {
            // Reset unico de las 14 en el primer submit: los writes de los
            // submits siguientes van detras en la misma cola.
            if (face == 0) vkCmdResetQueryPool(cmd, m_queryPool, 0, kQueryCount);
            vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, m_queryPool, face * 2);
        }

        // El mapa de AO que hay en la GPU es el de la camara del frame: dejarlo
        // hornearia oclusion de otro punto de vista dentro del cubemap. A 1.0
        // = sin oclusion; el frame siguiente lo recalcula si el SSAO esta
        // activo, y si no lo esta ya valia 1.0.
        if (face == 0 && ctx.ssaoBlurImage != VK_NULL_HANDLE)
        {
            // oldLayout UNDEFINED y no GENERAL: si el SSAO nunca ha corrido
            // sobre este slot la imagen no se ha transicionado nunca, y los
            // draws de aqui abajo la muestrean por el binding 7 (declarado
            // GENERAL). Descartar el contenido no cuesta nada: se limpia.
            VkImageMemoryBarrier ao{};
            ao.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            ao.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            ao.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            ao.image               = ctx.ssaoBlurImage;
            ao.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
            ao.newLayout           = VK_IMAGE_LAYOUT_GENERAL;
            ao.srcAccessMask       = 0;
            ao.dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
            ao.subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 0, 0, nullptr, 0, nullptr, 1, &ao);

            const VkClearColorValue white{{ 1.0f, 1.0f, 1.0f, 1.0f }};
            vkCmdClearColorImage(cmd, ctx.ssaoBlurImage, VK_IMAGE_LAYOUT_GENERAL,
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
        rpInfo.renderPass        = ctx.sceneRenderPass;
        rpInfo.framebuffer       = ctx.sceneFramebuffer;
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
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, ctx.scenePipeline);
        // Set 1: el SSBO de instancias sigue siendo obligatorio (el vertex
        // shader lo declara), aunque aqui no se instancie nada.
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
            ctx.scenePipelineLayout, 1, 1, &ctx.instanceSet, 0, nullptr);
        const VkDescriptorSet fpBakeSet = ctx.fp.set(0);
        if (fpBakeSet != VK_NULL_HANDLE)
        {
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                ctx.scenePipelineLayout, 2, 1, &fpBakeSet, 0, nullptr);
        }

        for (const auto& obj : ctx.objects)
        {
            const SharedGpuMesh* gpu = ctx.sharedMeshes.get(obj.sharedIndex);
            if (!gpu || gpu->uploadTicket > ctx.lastCompletedTicket) continue;
            if (!gpu->descriptorSets[0]) continue;
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                ctx.scenePipelineLayout, 0, 1, &gpu->descriptorSets[0], 0, nullptr);
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
            vkCmdPushConstants(cmd, ctx.scenePipelineLayout,
                VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                0, sizeof(PushData), &push);
            VkBuffer vbs[]      = { gpu->vertexBuffer };
            VkDeviceSize offs[] = { 0 };
            vkCmdBindVertexBuffers(cmd, 0, 1, vbs, offs);
            vkCmdBindIndexBuffer(cmd, gpu->indexBuffer, 0, VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexed(cmd, gpu->indexCount, 1, 0, 0, 0);
        }

        if (!ctx.skinnedObjects.empty())
        {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, ctx.skinnedPipeline);
            for (SkinnedRenderObject& sobj : ctx.skinnedObjects)
            {
                if (sobj.outputVertexBuffer == VK_NULL_HANDLE) continue;
                if (sobj.uploadTicket > ctx.lastCompletedTicket) continue;
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
                        ctx.scenePipelineLayout, 0, 1, &mgfx.descSets[0], 0, nullptr);
                    vkCmdPushConstants(cmd, ctx.scenePipelineLayout,
                        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                        0, sizeof(PushData), &push);
                    vkCmdDrawIndexed(cmd, sm.indexCount, 1, sm.indexStart, 0, 0);
                }
            }
        }

        if (ctx.skybox.isInitialized())
        {
            glm::mat4 rotView     = glm::mat4(glm::mat3(ubo.view));
            glm::mat4 invViewProj = glm::inverse(ubo.proj * rotView);
            ctx.skybox.draw(cmd, invViewProj);
        }

        vkCmdEndRenderPass(cmd);

        // ── Cara -> capa del cubemap de captura ──────────────────────────
        // El pass deja el HDR en SHADER_READ_ONLY (finalLayout del
        // attachment de resolve, y del de color sin MSAA).
        b.image            = ctx.hdrImage;
        b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        b.oldLayout        = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        b.newLayout        = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        b.srcAccessMask    = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        b.dstAccessMask    = VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);

        VkImageMemoryBarrier toDst = b;
        toDst.image            = m_captureImage;
        toDst.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, face, 1 };
        toDst.oldLayout        = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        toDst.newLayout        = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toDst.srcAccessMask    = VK_ACCESS_SHADER_READ_BIT;
        toDst.dstAccessMask    = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toDst);

        // Blit y no copy: el render sale a faceRender (recortado por el
        // tamano del viewport) y la cara del cubemap es siempre de
        // kFaceSize, asi que hay que escalar.
        VkImageBlit blit{};
        blit.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        blit.srcOffsets[0]  = { 0, 0, 0 };
        blit.srcOffsets[1]  = { (int32_t)faceRender, (int32_t)faceRender, 1 };
        blit.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, face, 1 };
        blit.dstOffsets[0]  = { 0, 0, 0 };
        blit.dstOffsets[1]  = { (int32_t)kFaceSize, (int32_t)kFaceSize, 1 };
        vkCmdBlitImage(cmd,
                       ctx.hdrImage,     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       m_captureImage,   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       1, &blit, VK_FILTER_LINEAR);

        // De vuelta a los layouts de partida: el HDR lo lee el bloom y
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

        if (ctx.timestampsSupported && m_queryPool != VK_NULL_HANDLE)
            vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, m_queryPool, face * 2 + 1);

        // Bloquea hasta que la cola vacia: el UBO del frame 0 se reescribe
        // en la vuelta siguiente y no puede pisarse un draw en vuelo.
        ctx.gpu.endOneTimeCommands(cmd);
    }

    // ── Convolucion: los MISMOS dos compute del IBL global ──────────────
    {
        vkResetDescriptorPool(ctx.gpu.device(), ctx.iblDescPool, 0);

        const uint32_t setCount = 1 + IblPass::kPrefilterMips;
        std::vector<VkDescriptorSetLayout> layouts(setCount, ctx.iblDescLayout);
        std::vector<VkDescriptorSet>       sets(setCount, VK_NULL_HANDLE);

        VkDescriptorSetAllocateInfo ai{};
        ai.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ai.descriptorPool     = ctx.iblDescPool;
        ai.descriptorSetCount = setCount;
        ai.pSetLayouts        = layouts.data();
        if (vkAllocateDescriptorSets(ctx.gpu.device(), &ai, sets.data()) != VK_SUCCESS)
            throw std::runtime_error("failed to allocate probe descriptor sets!");

        VkDescriptorImageInfo envInfo{};
        envInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        envInfo.imageView   = m_captureView;
        envInfo.sampler     = ctx.iblSampler;

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
        vkUpdateDescriptorSets(ctx.gpu.device(), (uint32_t)writes.size(), writes.data(), 0, nullptr);

        VkCommandBuffer cmd = ctx.gpu.beginOneTimeCommands();
        if (ctx.timestampsSupported && m_queryPool != VK_NULL_HANDLE)
            vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, m_queryPool, 12);

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
        conv[1].subresourceRange.levelCount = IblPass::kPrefilterMips;

        for (int i = 0; i < 2; i++)
        {
            conv[i].oldLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            conv[i].newLayout     = VK_IMAGE_LAYOUT_GENERAL;
            conv[i].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
            conv[i].dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        }
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             0, 0, nullptr, 0, nullptr, 2, conv);

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, ctx.iblIrradiancePipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, ctx.iblPipelineLayout,
                                0, 1, &sets[0], 0, nullptr);
        IblPass::Push push{ 0.0f, IblPass::kIrradianceSize, probe.intensity };
        vkCmdPushConstants(cmd, ctx.iblPipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
        const uint32_t irrGroups = (IblPass::kIrradianceSize + 7) / 8;
        vkCmdDispatch(cmd, irrGroups, irrGroups, 6);

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, ctx.iblPrefilterPipeline);
        for (uint32_t m = 0; m < IblPass::kPrefilterMips; m++)
        {
            const uint32_t mipSize = IblPass::kPrefilterSize >> m;
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, ctx.iblPipelineLayout,
                                    0, 1, &sets[1 + m], 0, nullptr);
            IblPass::Push mipPush{ (float)m / (float)(IblPass::kPrefilterMips - 1), mipSize, probe.intensity };
            vkCmdPushConstants(cmd, ctx.iblPipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(mipPush), &mipPush);
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

        if (ctx.timestampsSupported && m_queryPool != VK_NULL_HANDLE)
            vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, m_queryPool, 13);

        ctx.gpu.endOneTimeCommands(cmd);
    }

    if (restoreFp) ctx.fp.restoreParams(savedFp);

    // El UBO del frame 0 se queda con la ultima cara; updateUniformBuffer lo
    // reescribe entero antes del proximo submit del frame, asi que no hace
    // falta restaurarlo.

    probe.baked  = true;
    probe.bakeMs = 0.0f;
    if (ctx.timestampsSupported && m_queryPool != VK_NULL_HANDLE)
    {
        uint64_t stamps[kQueryCount] = {};
        // WAIT_BIT y no polling: la cola ya esta vacia (endOneTimeCommands
        // bloquea), asi que los 14 resultados estan listos.
        if (vkGetQueryPoolResults(ctx.gpu.device(), m_queryPool, 0, kQueryCount,
                                  sizeof(stamps), stamps, sizeof(uint64_t),
                                  VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) == VK_SUCCESS)
        {
            // Suma de los 7 deltas y no ultimo-menos-primero: entre submits
            // hay esperas del host que no son coste de GPU.
            double total = 0.0;
            for (uint32_t p = 0; p < kQueryCount / 2; p++)
                total += (double)(stamps[p * 2 + 1] - stamps[p * 2]) * ctx.timestampPeriod * 1e-6;
            probe.bakeMs = (float)total;
        }
    }
}

void ReflectionProbePass::sync(const Context& ctx)
{
    // Camino rapido: ni sondas en la escena ni nada que deshacer. Es el caso
    // de TODAS las escenas de hoy, y sale de aqui sin tocar la GPU.
    const bool nothingToDo = m_probes.empty() && m_assignShared.empty()
                          && m_assignSkinned.empty() && m_bakeQueue.empty()
                          && !m_bakeAllQueued;
    if (!ctx.scene && nothingToDo) return;

    // 1. Reconciliar la lista de sondas con la escena. Es lo unico que corre
    //    por frame cuando hay sondas: un recorrido del arbol (el mismo que
    //    ya hacen el gizmo y la fisica) y unas comparaciones de float.
    struct Desc { uint64_t id; glm::vec3 pos; float radius; float intensity; };
    std::vector<Desc> descs;
    if (ctx.scene)
    {
        ctx.scene->traverse([&](GameObject* go) {
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
        m_assignShared.clear();     // fuerza la reescritura de todos
        m_assignSkinned.clear();
        refreshAssignment(ctx);
        vkDeviceWaitIdle(ctx.gpu.device());
        destroyProbeImages(ctx, dying);
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
            createProbeImages(ctx, fresh);
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
    if (ctx.uboWritten)
    {
        const bool bakeAll = m_bakeAllQueued;
        m_bakeAllQueued = false;
        std::vector<uint64_t> queue;
        queue.swap(m_bakeQueue);

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
            assignAllToGlobalIbl(ctx);
            for (GpuProbe* p : toBake)
            {
                bake(ctx, *p);
                if (p->baked) { total += p->bakeMs; count++; }
            }
        }
        if (count > 0)
        {
            m_lastBakeMs = total;
            printf("reflection probes: bake de %d sonda(s) en %.2f ms de GPU "
                   "(captura %ux%u x6, irradiancia %ux%u, prefiltrado %ux%u x%u mips, "
                   "%.2f MB por sonda)\n",
                   count, total, kFaceSize, kFaceSize,
                   IblPass::kIrradianceSize, IblPass::kIrradianceSize,
                   IblPass::kPrefilterSize, IblPass::kPrefilterSize, IblPass::kPrefilterMips,
                   (double)probeMemoryBytes() / (1024.0 * 1024.0));
            fflush(stdout);
        }
    }

    // 3. Asignacion sonda->objeto. Sale sin escribir nada si no ha cambiado.
    refreshAssignment(ctx);
}

} // namespace DonTopo
