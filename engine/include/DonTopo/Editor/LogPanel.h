#pragma once
#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>

namespace DonTopo {

// Ring buffer de acciones de edición confirmadas, más reciente al final.
// Sin persistencia a disco (no hace falta guardar nada).
class LogPanel {
public:
    void push(const std::string& message);
    // Igual que push(message) pero etiquetando la entrada con un módulo
    // ("Renderer", "Physics", ...). El módulo se pinta como chip de color
    // delante del mensaje; el color sale de un hash del nombre, así que un
    // módulo nuevo no obliga a tocar el panel.
    void push(const std::string& message, const std::string& module);
    void draw();
    bool* GetOpenPtr() { return &m_open; }

    // Módulo de las entradas que llegan sin uno (todos los callers actuales
    // de pushLog): sin chip, se pintan exactamente igual que antes.
    static constexpr const char* kDefaultModule = "General";

private:
    struct Entry {
        std::string prefix;   // "[HH:MM:SS] "
        std::string message;
        std::string module;
        // La selección vive en la entrada, no en un índice aparte: así
        // sobrevive a los pop_front del ring buffer sin desplazarse.
        bool     selected = false;
        uint64_t id       = 0;
    };

    void drawRow(size_t index);
    void handleRowClick(size_t index);
    // Copia la selección al portapapeles; si no hay nada seleccionado, copia
    // todo lo que se está mostrando.
    void copySelection();

    static constexpr size_t kLogMaxEntries = 200;
    std::deque<Entry> m_entries;
    // Ancla del Shift+click, guardada por id (no por índice: el índice se
    // desplaza cuando el ring buffer descarta las entradas más viejas).
    uint64_t m_anchorId = 0;
    uint64_t m_nextId   = 1;
    // true si el panel ya estaba scrolleado al fondo el frame anterior —
    // evita pelear con el usuario si sube a leer historial mientras llegan
    // más líneas.
    bool m_autoScroll = true;
    bool m_open = true;
};

} // namespace DonTopo
