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
//  - descLayout()/descPool(): de ahi salen los descriptor sets de compute que
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
    struct Push
    {
        float    animTime;
        uint32_t boneCount;
        uint32_t vertexCount;
        // activeClip * boneCount: indice base del bloque del clip activo dentro
        // del SSBO de BoneInfos, que va en layout [clip][hueso]. Solo lo lee
        // bone_eval.comp; bone_hierarchy y skinning declaran este slot como
        // "pad" y no lo tocan.
        uint32_t clipBase;
        // --- Cross-fade ---
        // Segundo clip de la mezcla y su reloj. Con blendWeight >= 1 bone_eval
        // ni los mira, asi que el caso sin mezcla no paga la segunda
        // evaluacion. Solo los lee bone_eval.comp.
        float    prevAnimTime;
        uint32_t prevClipBase;
        // 0 = solo prevClip, 1 = solo el clip activo. El Animator manda 1
        // cuando no hay cross-fade en vuelo.
        float    blendWeight;
        // 1 = la traslacion del hueso raiz vuelve a la de su bind pose (clip
        // que desplaza el modelo reproducido en el sitio). Ocupa el slot que
        // antes era padding, asi que el bloque sigue en 32 bytes. Solo lo lee
        // bone_eval.comp.
        uint32_t lockRootMotion;
    };
    static_assert(sizeof(Push) == 32, "Push debe seguir en 32 bytes: los 3 .comp declaran este layout");

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
    VkDescriptorPool      descPool()   const { return m_descPool; }

private:
    VkDescriptorSetLayout m_descLayout       = VK_NULL_HANDLE;
    VkDescriptorPool      m_descPool         = VK_NULL_HANDLE;
    VkPipelineLayout      m_pipelineLayout   = VK_NULL_HANDLE;
    VkPipeline            m_boneEval         = VK_NULL_HANDLE;
    VkPipeline            m_boneHierarchy    = VK_NULL_HANDLE;
    VkPipeline            m_skinning         = VK_NULL_HANDLE;
};

} // namespace DonTopo
