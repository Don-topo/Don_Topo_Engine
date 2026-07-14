#pragma once
#include <cstddef>
#include <deque>
#include <memory>
#include <string>
#include "DonTopo/Editor/Command.h"

namespace DonTopo {

class UndoManager {
public:
    static constexpr size_t kMaxHistory = 50;

    // Registra cmd como ya-aplicado — el caller ejecuta la acción real ANTES
    // de llamar push(); push() nunca llama execute(). Vacía el redo stack
    // (una acción nueva invalida cualquier redo pendiente).
    void push(std::unique_ptr<ICommand> cmd);
    // No-op si el undo stack está vacío.
    void undo();
    // No-op si el redo stack está vacío.
    void redo();
    // Vacía ambos stacks — llamado en Load Scene y al entrar/salir de Play Mode.
    void clear();
    bool canUndo() const { return !m_undoStack.empty(); }
    bool canRedo() const { return !m_redoStack.empty(); }
    // Label del comando que acaba de deshacerse/rehacerse — solo válido
    // justo después de una llamada a undo()/redo() que sí hizo algo
    // (comprobar canUndo()/canRedo() antes de llamar).
    const std::string& lastLabel() const { return m_lastLabel; }

private:
    std::deque<std::unique_ptr<ICommand>> m_undoStack;
    std::deque<std::unique_ptr<ICommand>> m_redoStack;
    std::string m_lastLabel;
};

} // namespace DonTopo
