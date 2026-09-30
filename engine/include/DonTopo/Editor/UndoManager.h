#pragma once
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include "DonTopo/Editor/Command.h"

namespace DonTopo {

class UndoManager {
public:
    static constexpr size_t kMaxHistory = 50;

    // Registers cmd as already-applied. The caller runs the real action BEFORE
    // calling push(); push() never calls execute(). Empties the redo stack
    // (a new action invalidates any pending redo).
    //
    // dirtiesScene distinguishes the two families that share this history.
    // true (the default, and what the whole Properties panel does): the action
    // edits the SCENE, so it marks isSceneDirty. false: the action edits
    // settings that live in project.json and save themselves (those of the View
    // menu: bloom, SSAO, fog, AA...), and marking the scene dirty would make the
    // Content Browser ask to save a scene nobody has touched.
    //
    // They share a stack on purpose: Ctrl+Z undoes the user's last action
    // in its real order. Two histories would give two Ctrl+Z and no criterion
    // to choose which one handles the key.
    //
    // The flag never CLEARS the dirty state: a render setting behind a scene
    // edit does not turn that edit into a saved one.
    void push(std::unique_ptr<ICommand> cmd, bool dirtiesScene = true);
    // No-op if the undo stack is empty.
    void undo();
    // No-op if the redo stack is empty.
    void redo();
    // Empties both stacks. Called on Load Scene and when entering/leaving Play Mode.
    void clear();
    // Scene with unsaved changes. Set in push(), the only point that
    // all editor edits go through, and cleared with
    // markSceneSaved() when saving and when loading a scene from disk. clear() does NOT
    // touch it: emptying the history when entering/leaving Play Mode does not turn
    // the previous edits into saved ones.
    bool isSceneDirty() const { return m_sceneDirty; }
    void markSceneSaved() { m_sceneDirty = false; }
    bool canUndo() const { return !m_undoStack.empty(); }
    bool canRedo() const { return !m_redoStack.empty(); }
    // Changes every time the history moves: push, undo and redo that do
    // something, and clear. AnimatorGraphUndoTracker compares it between the start and
    // the end of a gesture to know whether the difference it sees in the graph is
    // only the user's or also from a foreign command (a Ctrl+Z in the middle of a
    // drag, a ClipRenameCommand from the panel itself, Play's clear()).
    uint64_t revision() const { return m_revision; }
    // Label of the command that was just undone/redone. Only valid
    // right after a call to undo()/redo() that did something
    // (check canUndo()/canRedo() before calling).
    const std::string& lastLabel() const { return m_lastLabel; }

private:
    std::deque<std::unique_ptr<ICommand>> m_undoStack;
    std::deque<std::unique_ptr<ICommand>> m_redoStack;
    std::string m_lastLabel;
    bool m_sceneDirty = false;
    uint64_t m_revision = 0;
};

} // namespace DonTopo
