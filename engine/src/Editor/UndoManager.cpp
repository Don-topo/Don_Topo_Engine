#include "DonTopo/Editor/UndoManager.h"

namespace DonTopo {

bool UndoManager::push(std::unique_ptr<ICommand> cmd, bool dirtiesScene)
{
    // Refused on its execute: nothing happened, nothing to undo, and the redo
    // stack is still valid.
    if (!cmd || !cmd->lastStepSucceeded()) return false;
    m_redoStack.clear();
    // With dirtiesScene=false the flag is NOT touched: it neither sets nor clears it. A
    // render setting pushed right after a scene edit must not make that edit look
    // saved.
    if (dirtiesScene) m_sceneDirty = true;
    m_undoStack.push_back(std::move(cmd));
    if (m_undoStack.size() > kMaxHistory)
        m_undoStack.pop_front();
    ++m_revision;
    return true;
}

void UndoManager::undo()
{
    if (m_undoStack.empty()) return;
    ICommand& cmd = *m_undoStack.back();
    cmd.undo();
    m_lastLabel   = cmd.label();
    m_lastMessage = cmd.lastStepMessage();
    m_lastRefused = !cmd.lastStepSucceeded();
    // Refused: it is still applied, so it stays on top of the undo stack and
    // nothing moved (no revision). Undoing what lies below it now would leave
    // the scene inconsistent.
    if (m_lastRefused) return;
    m_redoStack.push_back(std::move(m_undoStack.back()));
    m_undoStack.pop_back();
    ++m_revision;
}

void UndoManager::redo()
{
    if (m_redoStack.empty()) return;
    ICommand& cmd = *m_redoStack.back();
    cmd.execute();
    m_lastLabel   = cmd.label();
    m_lastMessage = cmd.lastStepMessage();
    m_lastRefused = !cmd.lastStepSucceeded();
    if (m_lastRefused) return;   // still undone: stays on the redo stack
    m_undoStack.push_back(std::move(m_redoStack.back()));
    m_redoStack.pop_back();
    ++m_revision;
}

void UndoManager::clear()
{
    m_undoStack.clear();
    m_redoStack.clear();
    m_lastLabel.clear();
    ++m_revision;
}

} // namespace DonTopo
