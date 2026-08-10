#pragma once
#include <string>
#include <vector>

namespace DonTopo {

// Ventana "Input Actions" — mapa de acciones con nombre a teclas, botones de
// ratón y botones de mando.
//
// Panel propio y no un bloque de Properties porque el mapa es global al
// proyecto, no de un GameObject: no hay selección de la que colgarlo.
//
// La captura de bindings usa directamente la API de teclas de ImGui
// (ImGuiKey_NamedKey_BEGIN..END, que ya incluye ImGuiKey_Mouse* y
// ImGuiKey_Gamepad*) en vez de una capa de input propia: el panel solo vive
// mientras el editor está en foco y ImGui ya tiene ahí los tres dispositivos.
class InputActionsPanel {
public:
    // Carga el JSON de persistencia si existe. Un fichero ausente o corrupto
    // deja el panel vacío, nunca aborta el arranque del editor.
    InputActionsPanel();

    void draw();
    bool* GetOpenPtr() { return &m_open; }
    void open() { m_open = true; }

    struct Action {
        std::string name;
        // Valores de ImGuiKey. Se guardan como int para no arrastrar <imgui.h>
        // a todo el que incluya este header (mismo criterio que el resto de
        // headers del editor).
        std::vector<int> bindings;
    };

private:
    bool load();   // devuelve false si no había fichero o no era legible
    void save() const;
    // Índice de la acción con ese nombre, o -1. Comparación exacta: dos
    // acciones que solo difieren en mayúsculas son dos acciones distintas.
    int findAction(const std::string& name) const;
    // Recorre el rango de ImGuiKey y devuelve la primera tecla/botón pulsado
    // este frame, o -1. Esc no se devuelve nunca: lo consume la cancelación.
    int pollFirstPressedKey() const;

    bool m_open = false;   // arranca cerrado: es un panel especializado

    std::vector<Action> m_actions;

    // Buffer de la barra de creación y del rename inline. ImGui necesita
    // almacenamiento estable entre frames, de ahí que sean miembros.
    char m_newNameBuf[64]  = {};
    char m_renameBuf[64]   = {};
    // Índice de la acción en rename, o -1. Índice y no nombre porque el rename
    // cambia justamente el nombre.
    int  m_renamingIndex = -1;

    // Índice de la acción a la espera de binding, o -1 si no hay escucha.
    int  m_listeningIndex = -1;

    // El fichero cargado venía sin el array "glfw" (anterior a las acciones en
    // scripting): se reescribe una vez, migrado, nada más cargarlo.
    bool m_needsMigrationSave = false;

    // Aviso mostrado en el propio panel (nombre duplicado, nombre vacío...).
    // Se limpia en cuanto la operación que lo provocó vuelve a intentarse.
    std::string m_warning;
};

} // namespace DonTopo
