#include "DonTopo/Editor/UndoManager.h"

namespace DonTopo {

void UndoManager::push(std::unique_ptr<ICommand> cmd)
{
    m_redoStack.clear();
    m_undoStack.push_back(std::move(cmd));
    if (m_undoStack.size() > kMaxHistory)
        m_undoStack.pop_front();
}

void UndoManager::undo()
{
    if (m_undoStack.empty()) return;
    std::unique_ptr<ICommand> cmd = std::move(m_undoStack.back());
    m_undoStack.pop_back();
    cmd->undo();
    m_lastLabel = cmd->label();
    m_redoStack.push_back(std::move(cmd));
}

void UndoManager::redo()
{
    if (m_redoStack.empty()) return;
    std::unique_ptr<ICommand> cmd = std::move(m_redoStack.back());
    m_redoStack.pop_back();
    cmd->execute();
    m_lastLabel = cmd->label();
    m_undoStack.push_back(std::move(cmd));
}

void UndoManager::clear()
{
    m_undoStack.clear();
    m_redoStack.clear();
    m_lastLabel.clear();
}

} // namespace DonTopo
