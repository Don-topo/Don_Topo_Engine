#pragma once
#include <vulkan/vulkan.h>
#include <glm/glm.hpp>
#include <string>
#include <vector>

namespace DonTopo {

    // Los tipos de instancia dibujable del backend Vulkan. Vivian dentro de
    // Renderer y salieron aqui por una razon concreta: ReflectionProbePass los
    // recibe en su Context para redibujar la escena en las seis caras de una
    // sonda, y meter Renderer.h dentro de un pase seria circular (Renderer.h ya
    // incluye el header del pase para tenerlo por valor).
    //
    // Estan en el mismo namespace que Renderer, asi que dentro de la clase se
    // siguen nombrando sin calificar exactamente igual que antes. Mismo
    // movimiento que se le hizo a Frustum cuando salio a Culling.

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
        // 0 = no refleja. Lo sincroniza el bucle de la aplicación desde
        // el GameObject, igual que el transform.
        float           ssrStrength         = 0.0f;
        // false = lo saltan los pases de escena, de sombras y de AO: el
        // mesh no se manda a la GPU, así que tampoco proyecta ni ocluye.
        bool            meshVisible         = true;
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
        // Fuerza de SSR del objeto, sincronizada desde el GameObject
        // igual que el transform. La ruta skinned dibuja una instancia
        // por submalla, así que aquí no hay agrupado que partir.
        float          ssrStrength          = 0.0f;
        // false = lo saltan el pass de escena y el de sombras. El compute
        // de skinning sí sigue corriendo: el contorno de selección lee
        // sus vértices de salida.
        bool           meshVisible          = true;
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

} // namespace DonTopo
