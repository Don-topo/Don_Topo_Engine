#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <nlohmann/json.hpp>
#include "DonTopo/Core/AnimatorComponent.h"

namespace DonTopo {

class ICommand;
class Scene;

// Turns into undo commands, one per gesture, the edits that the
// AnimatorPanel makes LIVE on the graph.
//
// The panel mutates the component in ~22 places. Wrapping each one with its
// before/after would be 22 obligations, and the 23rd place would skip it without
// warning. Instead, the panel calls beginFrame when it starts drawing and
// endFrame when it finishes, and this compares the graph between the two calls with the
// .scene key (animatorGraphKey). This way, anything that is saved and
// changes enters the undo, whichever place it comes from.
//
// No ImGui on purpose: who is active and at which revision the history is
// arrive as parameters, so it is tested without a GUI.
class AnimatorGraphUndoTracker {
public:
    // Opens a session if there is none open for this same GameObject: a drag
    // continuing from an earlier frame keeps its 'before'. A null anim discards.
    void beginFrame(uint64_t goId, const AnimatorComponent* anim, uint64_t undoRevision);

    // Returns the gesture's command when the graph has changed and no
    // widget is active any more, with the change ALREADY applied: the caller pushes it
    // without execute(). If the history moved during the gesture (different
    // undoRevision), it emits nothing and takes a new baseline: the difference
    // would include what another command did.
    std::unique_ptr<ICommand> endFrame(Scene& scene, const AnimatorComponent* anim,
                                       bool anyItemActive, uint64_t undoRevision);

    // Optional: name of the current gesture for the Log Console ("Delete
    // state"). If no place calls it, the command is called "Edit Animator".
    void setLabel(std::string label) { m_label = std::move(label); }

    // The panel has stopped drawing the graph (closed, collapsed, no
    // Animator): a session cannot survive that.
    void discard();

    bool sessionOpen() const { return m_open; }

private:
    static constexpr const char* kEtiquetaPorDefecto = "Edit Animator";

    void open(uint64_t goId, const AnimatorComponent& anim, uint64_t undoRevision);
    void close();

    bool                     m_open     = false;
    uint64_t                 m_id       = 0;
    uint64_t                 m_revision = 0;
    AnimatorComponent::Graph m_before;
    nlohmann::json           m_beforeKey;
    std::string              m_label    = kEtiquetaPorDefecto;
};

} // namespace DonTopo
