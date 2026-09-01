#pragma once
#include "DonTopo/Renderer/Frustum.h"
#include "DonTopo/Renderer/InstanceBatching.h"
#include "DonTopo/Renderer/RenderObjects.h"
#include "DonTopo/Renderer/SharedGpuMesh.h"

#include <cstdint>
#include <vector>

namespace DonTopo
{
    // Quién entra en un pase. La decisión estaba escrita CUATRO veces dentro
    // del backend de Vulkan —escena, sombras (por cascada), depth pre-pase y
    // contorno de selección— y las cuatro tenían que dar el mismo resultado:
    // si divergen, el AO oscurece contra geometría que no se dibuja, o queda
    // una sombra flotando sin objeto que la eche. Divergir ahí no rompe nada
    // que la capa de validación pueda ver.
    //
    // Sin nada de ninguna API gráfica más allá de los handles que ya trae
    // SharedGpuMesh, igual que Culling (Frustum.h) y Batching
    // (InstanceBatching.h): se puede ejercitar sin device.
    namespace Visibility
    {
        // Las cuatro guardas de siempre, en el mismo orden que tenían:
        //
        //  - obj.meshVisible: checkbox "Visible" del componente Mesh. Un mesh
        //    oculto no se manda a la GPU en NINGÚN pase, así que tampoco
        //    proyecta sombra ni ocluye.
        //  - !gpu: la entrada se borró desde el editor.
        //  - uploadTicket por delante del último completado: el upload sigue en
        //    vuelo, sus texturas están todavía en TRANSFER_DST_OPTIMAL y
        //    samplearlas sería leer basura. Aparece en cuanto la fence de su
        //    batch señale.
        //  - Fuera del frustum: no gasta ni ranura en el SSBO. Los objetos sin
        //    AABB (mesh vacío, hasBounds = false) no se pueden acotar y pasan
        //    siempre.
        //
        // El frustum lo elige el llamante: la cámara en el pase de escena, el
        // de ESTA cascada en el de sombras. Un objeto que la cámara no ve puede
        // seguir proyectando sombra sobre lo que sí se ve.
        inline bool objectVisible(const RenderObject& obj, const SharedGpuMesh* gpu,
                                  uint64_t lastCompletedTicket, const Culling::Frustum& frustum)
        {
            if (!obj.meshVisible || !gpu || gpu->uploadTicket > lastCompletedTicket)
                return false;

            if (gpu->hasBounds &&
                !Culling::aabbVisible(frustum, gpu->aabbMin, gpu->aabbMax, obj.transform))
            {
                return false;
            }

            return true;
        }

        // Evalúa TODOS los objetos y deja el resultado en `out`, listo para
        // Batching::buildInstanceBatches. Los invisibles también entran, con
        // visible = false: el agrupado los salta, pero el panel Performance los
        // cuenta como culleados.
        //
        // ssrEnabled = false deja la fuerza de SSR a 0 en todos. Es lo que
        // quieren los pases que no pintan color (sombras, profundidad): el SSR
        // entra en la clave del agrupado, así que con un único valor salen
        // menos draws y el mapa resultante es idéntico.
        inline void gatherCandidates(const std::vector<RenderObject>& objects,
                                     const SharedGpuMeshCache& meshes,
                                     uint64_t lastCompletedTicket,
                                     const Culling::Frustum& frustum,
                                     bool ssrEnabled,
                                     std::vector<Batching::BatchCandidate>& out)
        {
            out.clear();
            out.reserve(objects.size());
            for (const RenderObject& obj : objects)
            {
                const SharedGpuMesh* gpu = meshes.get(obj.sharedIndex);
                const bool visible = objectVisible(obj, gpu, lastCompletedTicket, frustum);
                const float ssr    = ssrEnabled ? obj.ssrStrength : 0.0f;
                out.push_back({ obj.sharedIndex, visible, &obj.transform, ssr });
            }
        }
    }
}
