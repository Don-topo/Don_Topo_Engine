#pragma once
#include <vulkan/vulkan.h>
#include <glm/glm.hpp>
#include <vector>
#include "DonTopo/Renderer/UniformBufferObject.h"

namespace DonTopo {

class GpuDevice;
class RendererState;

// Niebla volumetrica. El pase entero -pipeline, descriptor sets, queries de
// tiempo y grabacion- vive aqui; el Renderer sigue siendo el dueno de la
// instancia y el que decide CUANDO se llama a cada cosa.
class FogPass {
public:
    // Debe coincidir con Renderer::MAX_FRAMES (comprobado con static_assert en Renderer.cpp).
    static constexpr int kFramesInFlight = 2;

    // Lo que el pase necesita del Renderer y NO es suyo. Se construye en el
    // sitio de llamada y se pasa por referencia: nada de guardarlo, que los
    // handles se recrean con el swapchain.
    struct Context {
        GpuDevice&           gpu;
        const RendererState& state;
        // Resolucion INTERNA del render (la del HDR), no la del swapchain.
        const VkExtent2D&    renderExtent;
        int                  currentFrame;
        // La niebla no tiene imagen propia: reescribe el HDR in situ.
        const VkImage*       hdrImage;       // [kFramesInFlight]
        const VkImageView*   hdrView;        // [kFramesInFlight]
        const VkImageView*   ssaoDepthView;  // [kFramesInFlight]
        VkSampler            ssaoSampler;
        // El UBO del frame: matriz de vista, cortes y matrices de cascada.
        const VkBuffer*      uniformBuffers; // [kFramesInFlight]
        // El mismo par vista+sampler que muestrea pbr.frag.
        VkImageView          shadowView;
        VkSampler            shadowSampler;
        // La luz key es m_lights[0]; sin luces la niebla solo absorbe.
        const std::vector<Light>& lights;
        // A donde apunta una luz de PUNTO. El MISMO valor que recibieron las
        // cascadas este frame: si los dos no coinciden, el in-scattering apunta
        // a un lado y el shadow map esta construido hacia otro.
        glm::vec3            sceneCenter;
        // Los resolvio el bloom; aqui solo se leen.
        bool                 timestampsSupported;
        float                timestampPeriod;
    };

    FogPass()                          = default;
    FogPass(const FogPass&)            = delete;
    FogPass& operator=(const FogPass&) = delete;

    // Lo que no depende del tamano: layout, pool, pipeline y el pool de
    // queries. Una sola vez, en el init.
    void createPipelines(const Context& ctx);
    // Contrapartida de createPipelines, en el cleanup.
    void destroyPipelines(const Context& ctx);
    // Los sets van con el swapchain: referencian hdrView y ssaoDepthView, que
    // se recrean con el.
    void createSets(const Context& ctx);
    void destroySets();
    // Un solo dispatch que reescribe el HDR in situ. Va DESPUES del pass de
    // escena y del SSR -necesita el color ya iluminado y con los reflejos
    // sumados- y ANTES del bloom, para que la niebla florezca y pase por el
    // tonemap como el resto de la imagen.
    void record(const Context& ctx, VkCommandBuffer cmd, const glm::mat4& view, const glm::mat4& proj);

    // Coste GPU del dispatch en ms. 0 si esta apagada o el dispositivo no
    // soporta timestamps.
    float gpuMs() const { return m_gpuMs; }

private:
    VkDescriptorSetLayout m_descLayout            = VK_NULL_HANDLE;
    VkDescriptorPool      m_descPool              = VK_NULL_HANDLE;
    VkPipelineLayout      m_pipelineLayout        = VK_NULL_HANDLE;
    VkPipeline            m_pipeline              = VK_NULL_HANDLE;
    VkDescriptorSet       m_sets[kFramesInFlight] = {};
    // Dos queries por frame que acotan el unico dispatch. El depth pre-pass
    // NO entra aqui: ya lo miden el SSAO o el SSR cuando son ellos quienes lo
    // piden.
    VkQueryPool           m_queryPool                  = VK_NULL_HANDLE;
    bool                  m_queryPending[kFramesInFlight] = {};
    float                 m_gpuMs                      = 0.0f;
    uint32_t              m_measuredFrames             = 0;
};

} // namespace DonTopo
