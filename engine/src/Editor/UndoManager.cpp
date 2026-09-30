#include "DonTopo/Editor/UndoManager.h"

namespace DonTopo {

void UndoManager::push(std::unique_ptr<ICommand> cmd, bool dirtiesScene)
{
    m_redoStack.clear();
    // With dirtiesScene=false the flag is NOT touched: it neither sets nor clears it. A
    // render setting pushed right after a scene edit must not make that edit look
    // saved.
    if (dirtiesScene) m_sceneDirty = true;
    m_undoStack.push_back(std::move(cmd));
    if (m_undoStack.size() > kMaxHistory)
        m_undoStack.pop_front();
    ++m_revision;
}

void UndoManager::undo()
{
    if (m_undoStack.empty()) return;
    std::unique_ptr<ICommand> cmd = std::move(m_undoStack.back());
    m_undoStack.pop_back();
    cmd->undo();
    m_lastLabel = cmd->label();
    m_redoStack.push_back(std::move(cmd));
    ++m_revision;
}

void UndoManager::redo()
{
    if (m_redoStack.empty()) return;
    std::unique_ptr<ICommand> cmd = std::move(m_redoStack.back());
    m_redoStack.pop_back();
    cmd->execute();
    m_lastLabel = cmd->label();
    m_undoStack.push_back(std::move(cmd));
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
