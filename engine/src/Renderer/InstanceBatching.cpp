#include "DonTopo/Renderer/InstanceBatching.h"

#include <algorithm>

namespace DonTopo::Batching
{
    namespace
    {
        // The grouping key, in ONE place. The two passes below walk
        // the same chain looking for the same group, and while the comparison
        // was written twice, touching only one was enough for pass 2 to
        // put transforms in the wrong group: an object drawn with
        // another's material, with no error anywhere.
        //
        // sharedIndex does NOT take part: the chain already comes from slotOf[sharedIndex], so
        // all its links share it by construction.
        //
        // Exact float comparison on purpose: we are not looking for "similar" but for
        // "the same value", which is what guarantees that the group's push constant
        // holds for all its instances. Two values that differ in the
        // last bit have to end up in different draws.
        bool mismaClave(const InstanceBatch& b, const BatchCandidate& c)
        {
            return b.ssrStrength == c.ssr && b.metallic == c.metallic &&
                   b.roughness == c.roughness;
        }
    }

    uint32_t buildInstanceBatches(const BatchCandidate* candidates,
                                  size_t                count,
                                  glm::mat4*            outTransforms,
                                  uint32_t              outCapacity,
                                  uint32_t              firstInstanceBase,
                                  std::vector<InstanceBatch>& outBatches)
    {
        outBatches.clear();
        if (count == 0 || outCapacity == 0 || outTransforms == nullptr) return 0;

        // Table sharedIndex -> position in outBatches. The sharedIndex values are
        // dense, small indices of the cache, so a flat table avoids
        // the hash of an unordered_map (which in scenes of thousands of objects ate
        // up exactly what this grouping is meant to save).
        int maxShared = -1;
        for (size_t i = 0; i < count; i++)
            if (candidates[i].visible && candidates[i].sharedIndex > maxShared)
                maxShared = candidates[i].sharedIndex;
        if (maxShared < 0) return 0; // nothing is visible
        std::vector<int> slotOf((size_t)maxShared + 1, -1);
        // Chain of groups that share sharedIndex and differ in SSR strength,
        // parallel to outBatches. It is kept apart and not inside InstanceBatch
        // because it is grouping bookkeeping, not something the caller needs:
        // in the normal case (a single value per mesh) the chain has one
        // link and the result is identical to grouping only by sharedIndex.
        std::vector<int> nextOf;
        nextOf.reserve(count);

        // Pass 1: one group per (sharedIndex, ssr), in order of first
        // appearance.
        for (size_t i = 0; i < count; i++)
        {
            const BatchCandidate& c = candidates[i];
            if (!c.visible || c.sharedIndex < 0 || c.transform == nullptr) continue;
            int& first = slotOf[(size_t)c.sharedIndex];
            int  slot  = -1;
            for (int s = first; s >= 0; s = nextOf[(size_t)s])
            {
                if (mismaClave(outBatches[(size_t)s], c)) { slot = s; break; }
            }
            if (slot < 0)
            {
                slot = (int)outBatches.size();
                outBatches.push_back({ c.sharedIndex, 0, 0, c.ssr, c.metallic, c.roughness });
                nextOf.push_back(first);
                first = slot;
            }
            outBatches[(size_t)slot].instanceCount++;
        }

        // Contiguous offsets. If a group does not fit whole it is trimmed to what
        // remains and the following ones are left at zero: better to lose objects than to
        // write outside the buffer.
        uint32_t written = 0;
        for (auto& b : outBatches)
        {
            const uint32_t room = outCapacity - written;
            if (b.instanceCount > room) b.instanceCount = room;
            b.firstInstance = firstInstanceBase + written;
            written += b.instanceCount;
        }

        // Pass 2: contiguous transforms per group, in the order of the
        // candidates. cursor holds how many have already been written for each group, which
        // is also the relative slot within its range.
        std::vector<uint32_t> cursor(outBatches.size(), 0);
        for (size_t i = 0; i < count; i++)
        {
            const BatchCandidate& c = candidates[i];
            if (!c.visible || c.sharedIndex < 0 || c.transform == nullptr) continue;
            // Same lookup as in pass 1: the head of the chain is not necessarily
            // THIS candidate's group if they share a mesh and
            // differ in SSR strength.
            int found = -1;
            for (int s = slotOf[(size_t)c.sharedIndex]; s >= 0; s = nextOf[(size_t)s])
            {
                if (mismaClave(outBatches[(size_t)s], c)) { found = s; break; }
            }
            if (found < 0) continue;
            const size_t slot = (size_t)found;
            if (cursor[slot] >= outBatches[slot].instanceCount) continue; // trimmed group
            const uint32_t dst = (outBatches[slot].firstInstance - firstInstanceBase) + cursor[slot];
            outTransforms[dst] = *c.transform;
            cursor[slot]++;
        }

        // Groups that ran out of room must not arrive as draws of 0
        // instances.
        outBatches.erase(std::remove_if(outBatches.begin(), outBatches.end(),
                             [](const InstanceBatch& b) { return b.instanceCount == 0; }),
                         outBatches.end());
        return written;
    }
}  // namespace DonTopo::Batching
