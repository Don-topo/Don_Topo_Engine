#include "DonTopo/Renderer/InstanceBatching.h"

#include <algorithm>

namespace DonTopo::Batching
{
    uint32_t buildInstanceBatches(const BatchCandidate* candidates,
                                  size_t                count,
                                  glm::mat4*            outTransforms,
                                  uint32_t              outCapacity,
                                  uint32_t              firstInstanceBase,
                                  std::vector<InstanceBatch>& outBatches)
    {
        outBatches.clear();
        if (count == 0 || outCapacity == 0 || outTransforms == nullptr) return 0;

        // Tabla sharedIndex -> posición en outBatches. Los sharedIndex son
        // índices densos y pequeños de la caché, así que una tabla plana evita
        // el hash de un unordered_map (que en escenas de miles de objetos se
        // comía justo lo que este agrupado viene a ahorrar).
        int maxShared = -1;
        for (size_t i = 0; i < count; i++)
            if (candidates[i].visible && candidates[i].sharedIndex > maxShared)
                maxShared = candidates[i].sharedIndex;
        if (maxShared < 0) return 0; // no hay nada visible
        std::vector<int> slotOf((size_t)maxShared + 1, -1);
        // Cadena de grupos que comparten sharedIndex y difieren en la fuerza de
        // SSR, paralela a outBatches. Va aparte y no dentro de InstanceBatch
        // porque es contabilidad del agrupado, no algo que el llamante necesite:
        // en el caso normal (un único valor por malla) la cadena tiene un
        // eslabón y el resultado es idéntico al de agrupar solo por sharedIndex.
        std::vector<int> nextOf;
        nextOf.reserve(count);

        // Pasada 1: un grupo por (sharedIndex, ssr), en orden de primera
        // aparición.
        for (size_t i = 0; i < count; i++)
        {
            const BatchCandidate& c = candidates[i];
            if (!c.visible || c.sharedIndex < 0 || c.transform == nullptr) continue;
            int& first = slotOf[(size_t)c.sharedIndex];
            int  slot  = -1;
            for (int s = first; s >= 0; s = nextOf[(size_t)s])
            {
                if (outBatches[(size_t)s].ssrStrength == c.ssr) { slot = s; break; }
            }
            if (slot < 0)
            {
                slot = (int)outBatches.size();
                outBatches.push_back({ c.sharedIndex, 0, 0, c.ssr });
                nextOf.push_back(first);
                first = slot;
            }
            outBatches[(size_t)slot].instanceCount++;
        }

        // Offsets contiguos. Si un grupo no cabe entero se recorta a lo que
        // queda y los siguientes se quedan a cero: mejor perder objetos que
        // escribir fuera del buffer.
        uint32_t written = 0;
        for (auto& b : outBatches)
        {
            const uint32_t room = outCapacity - written;
            if (b.instanceCount > room) b.instanceCount = room;
            b.firstInstance = firstInstanceBase + written;
            written += b.instanceCount;
        }

        // Pasada 2: transforms contiguos por grupo, en el orden de los
        // candidatos. cursor lleva cuántos se han escrito ya de cada grupo, que
        // es también el hueco relativo dentro de su rango.
        std::vector<uint32_t> cursor(outBatches.size(), 0);
        for (size_t i = 0; i < count; i++)
        {
            const BatchCandidate& c = candidates[i];
            if (!c.visible || c.sharedIndex < 0 || c.transform == nullptr) continue;
            // Misma búsqueda que en la pasada 1: la cabeza de la cadena no tiene
            // por qué ser el grupo de ESTE candidato si comparten malla y
            // difieren en la fuerza de SSR.
            int found = -1;
            for (int s = slotOf[(size_t)c.sharedIndex]; s >= 0; s = nextOf[(size_t)s])
            {
                if (outBatches[(size_t)s].ssrStrength == c.ssr) { found = s; break; }
            }
            if (found < 0) continue;
            const size_t slot = (size_t)found;
            if (cursor[slot] >= outBatches[slot].instanceCount) continue; // grupo recortado
            const uint32_t dst = (outBatches[slot].firstInstance - firstInstanceBase) + cursor[slot];
            outTransforms[dst] = *c.transform;
            cursor[slot]++;
        }

        // Los grupos que se quedaron sin sitio no deben llegar como draws de 0
        // instancias.
        outBatches.erase(std::remove_if(outBatches.begin(), outBatches.end(),
                             [](const InstanceBatch& b) { return b.instanceCount == 0; }),
                         outBatches.end());
        return written;
    }
}  // namespace DonTopo::Batching
