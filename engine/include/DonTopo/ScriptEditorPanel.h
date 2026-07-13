#pragma once
#include <filesystem>
#include <functional>
#include <string>
#include <vector>
#include <TextEditor.h>

namespace DonTopo {

// Panel dockeable con tabs de ficheros .lua abiertos para edición manual.
// No conoce ScriptManager ni GameObject — solo lee/escribe texto en disco
// (FileManager). La recarga en la VM Lua la hace ScriptManager::pollChanges
// por su cuenta (mtime), este panel nunca llama loadScript.
class ScriptEditorPanel {
public:
    // No-op si path ya está abierto en alguna tab (esa tab pasa a tener foco).
    void openFile(const std::filesystem::path& path);
    void draw();
    // Fallos de lectura/escritura se reportan aquí en vez de silenciarse
    // (spec: deben verse en el Log Console del editor, pero este panel no
    // conoce EditorUI — EditorUI inyecta pushLog vía este callback).
    void setLogCallback(std::function<void(const std::string&)> cb) { m_log = std::move(cb); }

private:
    struct Tab {
        std::filesystem::path path;
        TextEditor editor;
        bool dirty = false;

        // Estado del popup de autocomplete (Task: diagnostics+autocomplete).
        bool acVisible = false;
        // true tras Escape, hasta que el fragmento bajo el cursor cambie —
        // evita que el popup se vuelva a abrir solo mientras se sigue
        // escribiendo la misma palabra que el usuario acaba de descartar.
        bool acDismissed = false;
        std::vector<std::string> acMatches;
        int acSelected = 0;
        TextEditor::Coordinates acFragmentStart;
        std::string acLastFragment;
        // Última posición de cursor observada — permite detectar movimiento
        // de caret (p.ej. click de ratón) que no dispara IsTextChanged(),
        // para cerrar el popup si queda con coordenadas obsoletas.
        TextEditor::Coordinates acLastCursor;
    };

    void saveTab(Tab& tab);
    void log(const std::string& msg) { if (m_log) m_log(msg); }

    std::vector<Tab> m_tabs;
    // Índice de tab a enfocar en el próximo draw() (-1 = ninguno); se consume
    // (vuelve a -1) tras cada frame.
    int m_focusIndex = -1;
    // Índice de tab con el popup "cambios sin guardar" pendiente (-1 = ninguno).
    int m_closeConfirmIndex = -1;
    bool m_openCloseConfirmPopup = false;
    std::function<void(const std::string&)> m_log;
};

} // namespace DonTopo
