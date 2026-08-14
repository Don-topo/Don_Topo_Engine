#pragma once
#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

namespace DonTopo
{
    // Agrupado de objetos en draws instanciados, sin nada de ninguna API
    // gráfica: lo usan el backend de Vulkan y el de DirectX 12. Vivía dentro de
    // Renderer (Vulkan) hasta que hubo un segundo backend que lo necesitaba,
    // igual que pasó con Culling (Renderer/Frustum.h).
    namespace Batching
    {
        // Un draw instanciado: todas las instancias del rango
        // [firstInstance, firstInstance + instanceCount) del buffer de
        // transforms comparten la entrada compartida sharedIndex, así que se
        // dibujan con un solo draw indexado.
        struct InstanceBatch {
            int      sharedIndex   = -1;
            uint32_t firstInstance = 0;
            uint32_t instanceCount = 0;
            // Fuerza de SSR común al grupo. Entra en la CLAVE de agrupado junto
            // a sharedIndex: metallic y roughness ya viajaban por entrada
            // compartida, así que dos objetos con la misma malla y distinta
            // fuerza de SSR no pueden compartir push constants. Solo se parten
            // en dos draws cuando los valores difieren.
            float    ssrStrength   = 0.0f;
        };

        // Un objeto ya evaluado por el pase que lo va a dibujar. Las guardas
        // (entrada borrada, upload en vuelo) y el culling por AABB los resuelve
        // el llamante —es quien tiene la caché GPU y el frustum— y llegan aquí
        // resumidos en `visible`. El transform va por puntero: el agrupado solo
        // lo copia al buffer, no lo guarda.
        struct BatchCandidate {
            int              sharedIndex = -1;
            bool             visible     = false;
            const glm::mat4* transform   = nullptr;
            // Los passes que no pintan color (sombras, depth pre-pass) lo dejan
            // a 0: con un único valor el agrupado sale idéntico al de antes de
            // la feature.
            float            ssr         = 0.0f;
        };

        // Agrupa por sharedIndex los candidatos VISIBLES y deja sus transforms
        // contiguos por grupo en outTransforms (que apunta ya al hueco del
        // buffer, con sitio para outCapacity matrices). El orden es estable: los
        // grupos salen por orden de primera aparición y dentro de cada grupo se
        // conserva el orden de los candidatos, de modo que el resultado no baila
        // entre frames.
        //
        // firstInstanceBase es el índice ABSOLUTO dentro del buffer de la
        // primera matriz escrita: los pases comparten buffer y el segundo
        // escribe detrás del primero, así que sin base los firstInstance del
        // pase principal apuntarían al rango de sombras.
        //
        // Devuelve cuántas matrices se han escrito. Si no caben todas trunca por
        // grupos (los que no entran salen con instanceCount 0 y se descartan)
        // antes que escribir fuera de rango: el llamante dimensiona el buffer
        // antes, esto es la red de seguridad.
        uint32_t buildInstanceBatches(const BatchCandidate* candidates,
                                      size_t                count,
                                      glm::mat4*            outTransforms,
                                      uint32_t              outCapacity,
                                      uint32_t              firstInstanceBase,
                                      std::vector<InstanceBatch>& outBatches);
    }  // namespace Batching
}  // namespace DonTopo
