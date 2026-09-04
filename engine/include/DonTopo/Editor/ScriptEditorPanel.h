#pragma once
#include <filesystem>
#include <functional>
#include <string>
#include <vector>
#include <TextEditor.h>
#include "DonTopo/Scripting/LuaApiReference.h"

namespace DonTopo {

// Panel dockeable con tabs de ficheros .lua abiertos para edición manual.
// No conoce ScriptManager ni GameObject — solo lee/escribe texto en disco
// (FileManager). La recarga en la VM Lua la hace ScriptManager::pollChanges
// por su cuenta (mtime), este panel nunca llama loadScript.
class ScriptEditorPanel {
public:
    // No-op si path ya está abierto en alguna tab (esa tab pasa a tener foco).
    // También reabre el panel si estaba cerrado (m_open = true) — abrir un
    // fichero desde Properties/Content Browser debe hacerlo visible.
    void openFile(const std::filesystem::path& path);
    void draw();
    // Fallos de lectura/escritura se reportan aquí en vez de silenciarse
    // (spec: deben verse en el Log Console del editor, pero este panel no
    // conoce EditorUI — EditorUI inyecta pushLog vía este callback).
    void setLogCallback(std::function<void(const std::string&)> cb) { m_log = std::move(cb); }
    // Puntero al flag de visibilidad de la ventana, usado por el checkbox
    // del menú View de EditorUI (ImGui::MenuItem togglea *bool directamente).
    bool* GetOpenPtr() { return &m_open; }

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
        // Fragmento exacto en el momento de Escape — permite distinguir
        // "seguir escribiendo la misma palabra" (extiende este prefijo,
        // se mantiene descartado) de "cambiar de palabra" (deja de
        // extenderlo, se vuelve a permitir el popup automático).
        std::string acDismissedFragment;
        // Sugerencias ya resueltas por luaApiMatches: cada una sabe qué texto
        // escribir y desde qué carácter del fragmento sustituirlo (una
        // sugerencia hallada por nombre de miembro conserva el 'variable:' que
        // el usuario escribió).
        std::vector<LuaApiMatch> acMatches;
        int acSelected = 0;
        TextEditor::Coordinates acFragmentStart;
        std::string acLastFragment;
        // Frames desde la última tecla. La comprobación de sintaxis en vivo
        // espera a que pare de escribir: recompilar el buffer en cada
        // pulsación pintaría errores en mitad de cada palabra a medio teclear.
        // -1 = nada pendiente.
        int syntaxDelay = -1;
        // Último error conocido (línea 1-based y mensaje); línea 0 = sin error.
        int errorLine = 0;
        std::string errorMessage;

        // Buscar / reemplazar. La barra se abre con Ctrl+F y se cierra con
        // Escape; el widget vendored no trae nada de esto.
        bool findOpen = false;
        bool findFocusRequested = false;
        char findBuffer[128] = {};
        char replaceBuffer[128] = {};
        bool findCaseSensitive = false;
        std::string findStatus;

        // Ir a línea (Ctrl+G).
        bool gotoOpen = false;
        int  gotoLine = 1;

        // mtime del fichero la última vez que lo leímos o escribimos. Sirve
        // para detectar que alguien lo ha cambiado por fuera (hot reload de
        // ScriptManager recarga la VM, pero esta pestaña seguiría con el texto
        // viejo y al guardar se llevaría por delante el cambio ajeno).
        std::filesystem::file_time_type diskTime {};
        bool externalChange = false;
        // Última posición de cursor observada — permite detectar movimiento
        // de caret (p.ej. click de ratón) que no dispara IsTextChanged(),
        // para cerrar el popup si queda con coordenadas obsoletas.
        TextEditor::Coordinates acLastCursor;
    };

    void saveTab(Tab& tab);
    // Recompila el buffer y actualiza marcador + estado de error de la barra.
    void refreshDiagnostics(Tab& tab);
    // Relee el fichero de disco, tirando lo que hubiera en el editor.
    void reloadFromDisk(Tab& tab);
    // Barra de buscar/reemplazar y salto a línea; devuelven true si han
    // consumido el teclado este frame (para no dárselo también al editor).
    bool drawFindBar(Tab& tab);
    void drawStatusBar(Tab& tab);
    // Busca 'needle' desde el cursor; envuelve al llegar al final. Selecciona
    // lo encontrado y devuelve true.
    bool findNext(Tab& tab, bool backwards);
    // Escribe la sugerencia elegida sustituyendo solo el trozo de fragmento
    // que le corresponde (ver LuaApiMatch::replaceOffset).
    void applyMatch(Tab& tab, const LuaApiMatch& match);
    void log(const std::string& msg) { if (m_log) m_log(msg); }

    std::vector<Tab> m_tabs;
    // Índice de tab a enfocar en el próximo draw() (-1 = ninguno); se consume
    // (vuelve a -1) tras cada frame.
    int m_focusIndex = -1;
    // Índice de tab con el popup "cambios sin guardar" pendiente (-1 = ninguno).
    int m_closeConfirmIndex = -1;
    bool m_openCloseConfirmPopup = false;
    // Visibilidad de la ventana del panel — togglable desde el menú View de
    // EditorUI vía GetOpenPtr(). No afecta a m_tabs: cerrar el panel solo
    // oculta la ventana, las tabs y su estado siguen en memoria.
    bool m_open = true;
    // Petición de traer la ventana al frente, puesta por openFile y consumida
    // por el siguiente draw(). No basta con m_open: si el panel está acoplado
    // en un grupo de pestañas, "abierto" solo quiere decir que la pestaña
    // existe, y el fichero se abría detrás de la pestaña que tuviera el foco.
    bool m_focusWindowRequested = false;
    std::function<void(const std::string&)> m_log;
};

} // namespace DonTopo
