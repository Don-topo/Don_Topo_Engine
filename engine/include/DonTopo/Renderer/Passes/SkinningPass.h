#pragma once
#include <vulkan/vulkan.h>
#include <vector>
#include <cstdint>
#include "DonTopo/Renderer/RenderObjects.h"

namespace DonTopo {

class GpuDevice;

// Skinning por compute: los tres dispatches que evaluan la animacion de cada
// malla con huesos (bone_eval -> bone_hierarchy -> skinning) y los recursos que
// comparten. La SALIDA es el outputVertexBuffer de cada objeto, que luego leen
// como vertex buffer el pass de escena, el de sombras y el contorno.
//
// Lo que NO es suyo: los cuatro pipelines GRAFICOS de las mallas skinned. Esos
// se quedan en el Renderer porque dependen del numero de muestras de MSAA y del
// render pass de escena, que gobiernan a varios pases.
//
// Ataduras con codigo que no es suyo:
//  - descLayout()/allocateSet(): de ahi salen los descriptor sets de compute que
//    aloja initSkinnedRenderObject y libera destroySkinnedRenderObject, que son
//    del Renderer porque van con los SSBOs de cada malla.
//  - los objetos y la lista de visibles llegan por el Context: son del
//    Renderer, y la lista de visibles es la MISMA que consume el bucle de
//    dibujo (saltar aqui un objeto que luego se dibuja le dejaria la pose del
//    ultimo frame en que fue visible).
class SkinningPass {
public:
    // ABI compartida por los 3 compute shaders. 32 bytes: los 4 primeros campos
    // no se han movido de sitio, los 4 del cross-fade se anadieron detras.
    // Espejo del bloque push_constant de los tres .comp de skinning (y de
    // ComputePush en D3D12): mismo orden, mismos tipos.
    struct Push
    {
        uint32_t boneCount;
        uint32_t vertexCount;
        // --- Solo los lee bone_eval.comp ---
        // Hasta 4 muestras (clip * boneCount, tiempo en ticks, peso) y el peso
        // de la pose congelada. Escalares y no arrays: en HLSL un array de un
        // cbuffer ocupa 16 bytes por elemento y spirv-cross no lo iguala.
        uint32_t sampleCount;
        uint32_t rootMotionMode;   // 0 libre, 1 raíz clavada a bind, 2 solo X y Z
        float    frozenWeight;
        uint32_t clipBase0, clipBase1, clipBase2, clipBase3;
        float    time0, time1, time2, time3;
        float    weight0, weight1, weight2, weight3;
    };
    static_assert(sizeof(Push) == 68, "Push: los 3 .comp y ComputePush de D3D12 declaran este layout");

    struct Context {
        GpuDevice& gpu;
        // Las mallas con huesos y la lista de visibles del frame, las dos del
        // Renderer.
        std::vector<SkinnedRenderObject>& skinnedObjects;
        const std::vector<uint8_t>&       skinnedVisible;
    };

    SkinningPass()                               = default;
    SkinningPass(const SkinningPass&)            = delete;
    SkinningPass& operator=(const SkinningPass&) = delete;

    // Set layout, pool, pipeline layout y los tres pipelines compute. Una sola
    // vez, en el init: nada de esto depende del tamano ni de las muestras.
    void createPipelines(const Context& ctx);
    void destroyPipelines(const Context& ctx);

    // Los tres dispatches por objeto visible, con sus barreras. Va al principio
    // del command buffer del frame, ANTES del pass de sombras: la ultima
    // barrera es compute -> VERTEX_INPUT, que es lo que deja el
    // outputVertexBuffer listo para dibujarse.
    void record(const Context& ctx, VkCommandBuffer cmd);

    // De aqui salen los descriptor sets de compute de cada malla, que aloja y
    // libera el Renderer junto con los SSBOs del objeto.
    VkDescriptorSetLayout descLayout() const { return m_descLayout; }

    // Aloja un set y devuelve el POOL del que salio, que es lo que hace falta
    // despues para liberarlo (vkFreeDescriptorSets pide el pool concreto). Si
    // el ultimo pool esta lleno, crea otro y sigue: antes habia uno solo con
    // maxSets = 16 y el personaje 17 hacia lanzar en mitad de la carga de
    // escena.
    //
    // Se crece encadenando pools en vez de recreando uno mas grande a proposito:
    // recrearlo invalidaria los sets de todas las mallas ya cargadas, y habria
    // que rehacerlos todos con la GPU parada.
    //
    // Devuelve VK_NULL_HANDLE si ni siquiera se pudo crear el pool nuevo.
    VkDescriptorPool allocateSet(const Context& ctx, VkDescriptorSet& outSet);

private:
    // Crea un pool mas de la cadena. kSetsPerPool sale del tope historico: la
    // mayoria de escenas caben en el primero y no se paga nada.
    static constexpr uint32_t kSetsPerPool = 16;
    bool addPool(const Context& ctx);

    VkDescriptorSetLayout m_descLayout       = VK_NULL_HANDLE;
    std::vector<VkDescriptorPool> m_descPools;
    VkPipelineLayout      m_pipelineLayout   = VK_NULL_HANDLE;
    VkPipeline            m_boneEval         = VK_NULL_HANDLE;
    VkPipeline            m_boneHierarchy    = VK_NULL_HANDLE;
    VkPipeline            m_skinning         = VK_NULL_HANDLE;
};

} // namespace DonTopo
