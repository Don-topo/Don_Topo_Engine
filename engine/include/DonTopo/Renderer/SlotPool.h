#pragma once
#include <cstddef>
#include <vector>

namespace DonTopo {

// Free slots of a vector of render objects.
//
// Both backends keep their objects in a vector and record the index inside the
// GameObject (`staticRenderIndex` / `skinnedRenderIndex`). That prevents
// COMPACTING on delete (moving one entry would leave all the others pointing to
// another mesh), so the hole stays there and the vector only grows: a
// Play/Stop cycle added slots forever (H19, H43). With the pool, deleting
// returns the index and the next add reuses it, so the indices of
// others keep meaning the same thing.
//
// In D3D12 recycling the index also recycles its descriptor block, because
// the block is derived from the index (`kSrvObjects + index * kSrvPerObject`).
//
// Header-only and dependency-free: used by both backends and their tests.
class SlotPool {
public:
    // A free slot, or -1 if there is none: then the caller grows its vector,
    // which is what was always done before this. LIFO: the last one freed
    // is the first to come back.
    int acquire()
    {
        if (m_free.empty())
            return -1;
        const int slot = m_free.back();
        m_free.pop_back();
        if (static_cast<size_t>(slot) < m_isFree.size())
            m_isFree[static_cast<size_t>(slot)] = false;
        return slot;
    }

    // Returns the slot. Ignores negatives (an unassigned *RenderIndex) and
    // those that are ALREADY free.
    //
    // That second case is the reason for keeping `m_isFree` and not just the stack: a
    // double release (removing a subtree that had already been removed) would leave the
    // same index twice in the list, and two new objects would end up
    // sharing a slot and a descriptor block. Not even the validation layer
    // would warn about it: both indices are valid, they just are the same one.
    void release(int slot)
    {
        if (slot < 0)
            return;
        const size_t i = static_cast<size_t>(slot);
        if (i >= m_isFree.size())
            m_isFree.resize(i + 1, false);
        if (m_isFree[i])
            return;
        m_isFree[i] = true;
        m_free.push_back(slot);
    }

    // The object vectors have been emptied entirely (clearStaticMeshes, or
    // shutdown): no previous slot is valid any more.
    void clear()
    {
        m_free.clear();
        m_isFree.clear();
    }

    size_t freeCount() const { return m_free.size(); }

private:
    std::vector<int>  m_free;
    // Indexed by slot: true = it is in m_free. Only to reject the double
    // release; it grows up to the highest index that has been freed.
    std::vector<bool> m_isFree;
};

} // namespace DonTopo
