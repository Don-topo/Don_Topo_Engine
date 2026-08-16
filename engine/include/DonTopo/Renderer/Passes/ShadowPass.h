#pragma once
#include <vulkan/vulkan.h>
#include <glm/glm.hpp>
#include <vector>
#include "DonTopo/Renderer/UniformBufferObject.h"

namespace DonTopo {

class GpuDevice;
struct Light;

// Shadow map de cascadas: el texture array (una capa por cascada), su sampler
// de comparacion, el render pass depth-only, los framebuffers por capa, los dos
// pipelines (estatico y skinned) y el reparto de cascadas.
//
// Mismo reparto que DepthPrepassPass: esta clase posee el TARGET y los
// PIPELINES; los DRAWS se quedan en el Renderer, entre beginCascade() y
// endCascade(), porque salen de sus listas de objetos y de su SSBO de
// instancias — y el cursor de ese SSBO lo comparten el pass de sombras, el
// depth pre-pass y el de escena.
//
// Ataduras con codigo que no es suyo:
//  - view()/sampler(): van al binding 3 de cada descriptor set de objeto, que
//    escriben allocateObjectDescriptorSet y la ruta skinned, y al Context de la
//    niebla.
//  - pipelineLayout(): lo PRESTA el depth pre-pass, que declara los mismos dos
//    sets y no usa el push constant.
//  - cascadeMatrices()/cascadeSplits(): los copia updateUniformBuffer al UBO,
//    que es donde los lee pbr.frag.
class ShadowPass {
public:
    // Lado del mapa, en texeles. El numero de cascadas es SHADOW_CASCADES y
    // vive en UniformBufferObject.h: tiene que valer lo mismo aqui, en el array
    // del bloque UBO de los shaders y en las capas de este texture array.
    static constexpr uint32_t kShadowSize = 2048;

    struct Context {
        GpuDevice& gpu;
        // Los dos sets que declara shadow.vert: el del objeto (UBO + texturas,
        // aunque este shader solo lea el UBO) y el SSBO de instancias. Los dos
        // son del Renderer; el pipeline layout que sale de ellos es lo que
        // luego toma prestado el depth pre-pass.
        VkDescriptorSetLayout objectSetLayout;
        VkDescriptorSetLayout instanceSetLayout;
    };

    ShadowPass()                             = default;
    ShadowPass(const ShadowPass&)            = delete;
    ShadowPass& operator=(const ShadowPass&) = delete;

    // Imagen, vistas, sampler, render pass, framebuffers y los dos pipelines.
    // Una sola vez, en el init. Nada de esto depende del tamano de la ventana.
    void createResources(const Context& ctx);
    void destroyResources(const Context& ctx);

    // Reparto de cascadas y matriz de cada una, a partir del frustum de la
    // camara del frame y de la primera luz. Una vez por frame, ANTES de
    // updateUniformBuffer y de la grabacion: los dos consumidores leen la cache.
    // Sin luces (o con una camara degenerada) deja identidad y splits a 0, que
    // es lo que apaga el muestreo en el shader.
    void computeCascades(const glm::mat4& view, const glm::mat4& proj,
                         const std::vector<Light>& lights);

    // Abre el render pass de una cascada con el viewport, el scissor, el
    // pipeline estatico y el push del indice ya puestos. Entre esto y
    // endCascade() el Renderer graba sus draws.
    //
    // Se llama SIEMPRE, tambien sin luces: es este render pass el que limpia la
    // capa y la deja en DEPTH_STENCIL_READ_ONLY_OPTIMAL, que es el layout que
    // declaran los descriptor sets. Lo que el Renderer se salta en ese caso son
    // los draws.
    void beginCascade(VkCommandBuffer cmd, uint32_t cascade);
    // Cambia al pipeline de las mallas skinned dentro de la misma cascada. El
    // layout es el mismo, asi que el push del indice sobrevive al cambio; se
    // reescribe por no depender de esa compatibilidad.
    void bindSkinnedPipeline(VkCommandBuffer cmd, uint32_t cascade);
    void endCascade(VkCommandBuffer cmd);

    // Vista del array completo: la que muestrea pbr.frag (sampler2DArrayShadow)
    // y la que va en los descriptor sets.
    VkImageView      view()           const { return m_view; }
    VkSampler        sampler()        const { return m_sampler; }
    VkPipelineLayout pipelineLayout() const { return m_pipelineLayout; }

    const glm::mat4* cascadeMatrices() const { return m_cascadeMatrices; }  // [SHADOW_CASCADES]
    const glm::mat4& cascadeMatrix(uint32_t c) const { return m_cascadeMatrices[c]; }
    const glm::vec4& cascadeSplits()   const { return m_cascadeSplits; }

private:
    VkImage        m_image                          = VK_NULL_HANDLE;
    VkDeviceMemory m_memory                         = VK_NULL_HANDLE;
    VkImageView    m_view                           = VK_NULL_HANDLE;
    // Una vista de UNA capa por cascada. Solo existen para poder colgar un
    // framebuffer de cada capa; nadie las muestrea.
    VkImageView    m_layerViews[SHADOW_CASCADES]    {};
    VkSampler      m_sampler                        = VK_NULL_HANDLE;
    VkRenderPass   m_renderPass                     = VK_NULL_HANDLE;
    VkFramebuffer  m_framebuffers[SHADOW_CASCADES]  {};
    VkPipeline     m_pipeline                       = VK_NULL_HANDLE;
    // Hermano del anterior para las mallas skinned: mismo shadow.vert, mismo
    // layout, mismo render pass y mismo bias. Solo cambia el vertex input,
    // porque lo que se dibuja es la salida del compute de skinning (5xvec4,
    // stride 80) y el stride es estado de pipeline.
    VkPipeline       m_skinnedPipeline = VK_NULL_HANDLE;
    VkPipelineLayout m_pipelineLayout  = VK_NULL_HANDLE;

    // Cache por frame que rellena computeCascades(). Identidad y 0 si la escena
    // no tiene luces: en ese caso el pass solo limpia las capas.
    glm::mat4 m_cascadeMatrices[SHADOW_CASCADES] { glm::mat4(1.0f), glm::mat4(1.0f),
                                                   glm::mat4(1.0f), glm::mat4(1.0f) };
    glm::vec4 m_cascadeSplits { 0.0f };
};

} // namespace DonTopo
