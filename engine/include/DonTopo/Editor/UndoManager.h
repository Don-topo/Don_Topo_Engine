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
    //
    // dirtiesScene distingue las dos familias que comparten este historial.
    // true (el default, y lo que hace todo el panel Properties): la acción
    // edita la ESCENA, así que marca isSceneDirty. false: la acción edita
    // ajustes que viven en el project.json y se guardan solos —los del menú
    // View: bloom, SSAO, niebla, AA…—, y marcar la escena como sucia haría que
    // el Content Browser pidiera guardar una escena que nadie ha tocado.
    //
    // Comparten stack a propósito: Ctrl+Z deshace la última acción del usuario
    // en su orden real. Dos historiales darían dos Ctrl+Z y ningún criterio
    // para elegir cuál atiende la tecla.
    //
    // El flag nunca LIMPIA el dirty: un ajuste de render detrás de una edición
    // de escena no convierte esa edición en guardada.
    void push(std::unique_ptr<ICommand> cmd, bool dirtiesScene = true);
    // No-op si el undo stack está vacío.
    void undo();
    // No-op si el redo stack está vacío.
    void redo();
    // Vacía ambos stacks — llamado en Load Scene y al entrar/salir de Play Mode.
    void clear();
    // Escena con cambios sin guardar. Se marca en push() —el único punto por
    // el que pasan todas las ediciones del editor— y se limpia con
    // markSceneSaved() al guardar y al cargar una escena de disco. clear() NO
    // lo toca: vaciar el historial al entrar/salir de Play Mode no convierte
    // en guardadas las ediciones previas.
    bool isSceneDirty() const { return m_sceneDirty; }
    void markSceneSaved() { m_sceneDirty = false; }
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
    bool m_sceneDirty = false;
};

} // namespace DonTopo
