#pragma once
#include <deque>
#include <string>

namespace DonTopo {

// Ring buffer de acciones de edición confirmadas, más reciente al final.
// Sin persistencia a disco (no hace falta guardar nada).
class LogPanel {
public:
    void push(const std::string& message);
    void draw();
    bool* GetOpenPtr() { return &m_open; }

private:
    static constexpr size_t kLogMaxEntries = 200;
    std::deque<std::string> m_entries;
    // true si el panel ya estaba scrolleado al fondo el frame anterior —
    // evita pelear con el usuario si sube a leer historial mientras llegan
    // más líneas.
    bool m_autoScroll = true;
    bool m_open = true;
};

} // namespace DonTopo
