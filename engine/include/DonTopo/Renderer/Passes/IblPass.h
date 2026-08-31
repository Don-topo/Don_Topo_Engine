#pragma once
#include "DonTopo/Renderer/RenderConstants.h"
#include <vulkan/vulkan.h>
#include <cstdint>

namespace DonTopo {

class GpuDevice;

// IBL global: dos cubemaps precomputados UNA vez sobre el cubemap del skybox,
// irradiancia (difuso) y entorno prefiltrado por rugosidad (mips). El termino
// BRDF no es una textura: pbr.frag usa la aproximacion analitica de Karis, asi
// que no hay LUT ni un tercer binding.
//
// Las imagenes se crean SIEMPRE en el init, con contenido neutro, y solo se
// rellenan de verdad si initSkybox() ha cargado un cubemap. Asi los descriptor
// sets nunca apuntan a un handle nulo y una escena sin skybox se ilumina con un
// ambiente plano en vez de reventar.
//
// Ataduras con codigo que no es suyo:
//  - irradianceView()/prefilterView()/sampler(): los escriben en los bindings 5
//    y 6 de sus descriptor sets allocateObjectDescriptorSet y la ruta skinned,
//    que son del Renderer.
//  - los dos pipelines de convolucion, su layout, su pool y su set layout los
//    REUSA ReflectionProbePass para convolucionar la captura de cada sonda: por
//    eso salen a la interfaz publica, por handle.
class IblPass {
public:
    // Los tres salen de RenderConstants.h: el backend D3D12 los necesita
    // IGUALES y los tenia copiados con su valor a fuego. Aqui se re-exponen con
    // el nombre que ya usaba este pase, para no tocar sus llamantes.
    //
    // kPrefilterMips vive ademas como #define IBL_PREFILTER_MIPS en
    // shaders/pbr.frag, y esa tercera copia NO se puede compartir: un shader no
    // incluye un header de C++, y meterlo en el bloque UBO lo desplazaria en
    // silencio para los seis shaders que lo declaran (std140).
    static constexpr uint32_t kIrradianceSize = IBL_IRRADIANCE_SIZE;
    static constexpr uint32_t kPrefilterSize  = IBL_PREFILTER_SIZE;
    static constexpr uint32_t kPrefilterMips  = IBL_PREFILTER_MIPS;
    // rgba16f: los cubemaps son HDR. Con 8 bits el especular prefiltrado se
    // bandearia en las zonas de gradiente suave.
    static constexpr VkFormat kFormat = VK_FORMAT_R16G16B16A16_SFLOAT;

    // Push de ibl_irradiance.comp y de ibl_prefilter.comp.
    // intensity: peso que se hornea en el cubemap resultante. 1.0 en el IBL
    // global (resultado identico al de antes de las sondas) y la intensidad de
    // la probe cuando esto convoluciona su captura.
    struct Push { float roughness; uint32_t faceSize; float intensity; };

    struct Context {
        GpuDevice& gpu;
        // Cubemap de entorno del skybox. VK_NULL_HANDLE = no hay entorno
        // cargado: precompute() no hace nada y los dos cubemaps se quedan con
        // el contenido neutro que dejo createResources().
        VkImageView envView;
        VkSampler   envSampler;
    };

    IblPass()                          = default;
    IblPass(const IblPass&)            = delete;
    IblPass& operator=(const IblPass&) = delete;

    // Imagenes, vistas, sampler, layout, pool y los dos pipelines compute, mas
    // el clear al ambiente neutro. Una sola vez, en el init.
    void createResources(const Context& ctx);
    void destroyResources(const Context& ctx);

    // Rellena los dos cubemaps a partir del cubemap del skybox. No-op si no hay
    // entorno. Una sola vez, desde initSkybox().
    void precompute(const Context& ctx);

    // Un write suelto de los bindings 5 y 6 sobre un set YA alojado, igual que
    // writeSsaoBinding: reescribirlos es lo unico que hace falta para que un
    // objeto pase del IBL global a una sonda. Ni layout nuevo, ni miembro nuevo
    // en el UBO, ni un indice en PushData (que esta a 80 bytes justos).
    void writeBindings(const Context& ctx, VkDescriptorSet set,
                       VkImageView irradiance, VkImageView prefilter) const;

    // Las dos vistas CUBE que van en los descriptor sets de cada objeto.
    VkImageView irradianceView() const { return m_irradianceView; }
    VkImageView prefilterView()  const { return m_prefilterView; }
    VkSampler   sampler()        const { return m_sampler; }

    // Lo que reusa el bake de las sondas.
    VkPipeline            irradiancePipeline() const { return m_irradiancePipeline; }
    VkPipeline            prefilterPipeline()  const { return m_prefilterPipeline; }
    VkPipelineLayout      pipelineLayout()     const { return m_pipelineLayout; }
    VkDescriptorPool      descPool()           const { return m_descPool; }
    VkDescriptorSetLayout descLayout()         const { return m_descLayout; }

private:
    VkImage        m_irradianceImage  = VK_NULL_HANDLE;
    VkDeviceMemory m_irradianceMemory = VK_NULL_HANDLE;
    // Vista CUBE pa muestrear desde pbr.frag; vista 2D_ARRAY pa que el compute
    // pueda escribirla como storage image (un imageCube de escritura exigiria
    // capacidades que no hacen falta).
    VkImageView    m_irradianceView   = VK_NULL_HANDLE;
    VkImageView    m_irradianceStore  = VK_NULL_HANDLE;
    VkImage        m_prefilterImage   = VK_NULL_HANDLE;
    VkDeviceMemory m_prefilterMemory  = VK_NULL_HANDLE;
    VkImageView    m_prefilterView    = VK_NULL_HANDLE;
    VkImageView    m_prefilterStore[kPrefilterMips] {};
    VkSampler      m_sampler          = VK_NULL_HANDLE;

    VkDescriptorSetLayout m_descLayout          = VK_NULL_HANDLE;
    VkDescriptorPool      m_descPool            = VK_NULL_HANDLE;
    VkPipelineLayout      m_pipelineLayout      = VK_NULL_HANDLE;
    VkPipeline            m_irradiancePipeline  = VK_NULL_HANDLE;
    VkPipeline            m_prefilterPipeline   = VK_NULL_HANDLE;
};

} // namespace DonTopo
