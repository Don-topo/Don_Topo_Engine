#include "DonTopo/Editor/AnimatorGraphUndo.h"
#include "DonTopo/Core/AnimatorSerialization.h"
#include "DonTopo/Editor/Command.h"

namespace DonTopo {

void AnimatorGraphUndoTracker::open(uint64_t goId, const AnimatorComponent& anim,
                                    uint64_t undoRevision)
{
    m_open      = true;
    m_id        = goId;
    m_revision  = undoRevision;
    m_before    = anim.graph();
    m_beforeKey = animatorGraphKey(anim);
}

void AnimatorGraphUndoTracker::close()
{
    m_open  = false;
    m_label = kEtiquetaPorDefecto;
}

void AnimatorGraphUndoTracker::discard()
{
    close();
}

void AnimatorGraphUndoTracker::beginFrame(uint64_t goId, const AnimatorComponent* anim,
                                          uint64_t undoRevision)
{
    if (!anim) { close(); return; }
    if (m_open && m_id == goId) return;   // the gesture continues from an earlier frame
    open(goId, *anim, undoRevision);
}

std::unique_ptr<ICommand> AnimatorGraphUndoTracker::endFrame(Scene& scene,
                                                             const AnimatorComponent* anim,
                                                             bool anyItemActive,
                                                             uint64_t undoRevision)
{
    if (!m_open) return nullptr;
    if (!anim) { close(); return nullptr; }

    if (undoRevision != m_revision)
    {
        // Someone moved the history in the middle of the gesture. New baseline: if the
        // gesture continues (a drag), what remains of it will be measured from here.
        open(m_id, *anim, undoRevision);
        m_label = kEtiquetaPorDefecto;
        if (!anyItemActive) close();
        return nullptr;
    }

    if (animatorGraphKey(*anim) == m_beforeKey) { close(); return nullptr; }
    if (anyItemActive) return nullptr;   // the drag continues: a single command on release

    auto cmd = std::make_unique<AnimatorGraphCommand>(scene, m_label, m_id,
                                                      std::move(m_before), anim->graph());
    close();
    return cmd;
}

} // namespace DonTopo
