#include "DonTopo/Editor/LogPanel.h"
#include <imgui.h>
#include <cctype>
#include <chrono>
#include <ctime>

namespace DonTopo {

namespace {

// Color del chip a partir de un hash estable (FNV-1a) del nombre del módulo:
// mismo módulo = mismo color entre ejecuciones, y un módulo nuevo no obliga a
// tocar ninguna tabla. Solo se hashea el nombre para elegir el tono; la
// saturación y el valor son fijos para que todos los chips sean legibles.
ImVec4 moduleColor(const std::string& module)
{
    uint32_t h = 2166136261u;
    for (unsigned char c : module) {
        h ^= c;
        h *= 16777619u;
    }
    const float hue = static_cast<float>(h % 360u) / 360.0f;
    float r = 0.0f, g = 0.0f, b = 0.0f;
    ImGui::ColorConvertHSVtoRGB(hue, 0.55f, 0.80f, r, g, b);
    return ImVec4(r, g, b, 1.0f);
}

// Texto negro sobre chips claros, blanco sobre los oscuros.
ImVec4 chipTextColor(const ImVec4& bg)
{
    const float luma = 0.299f * bg.x + 0.587f * bg.y + 0.114f * bg.z;
    return luma > 0.6f ? ImVec4(0.05f, 0.05f, 0.05f, 1.0f) : ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
}

// Chip de módulo: rectángulo redondeado con el nombre dentro. Deja el cursor
// listo para que el caller siga con SameLine(0, 0).
void drawModuleChip(const std::string& module)
{
    const ImVec4 bg  = moduleColor(module);
    const ImVec2 sz  = ImGui::CalcTextSize(module.c_str());
    const ImVec2 p0  = ImGui::GetCursorScreenPos();
    const float  pad = 4.0f;
    ImGui::GetWindowDrawList()->AddRectFilled(
        p0, ImVec2(p0.x + sz.x + pad * 2.0f, p0.y + sz.y), ImGui::ColorConvertFloat4ToU32(bg), 3.0f);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + pad);
    ImGui::TextColored(chipTextColor(bg), "%s", module.c_str());
    ImGui::SameLine(0.0f, 0.0f);
    // Padding derecho del chip + separación con el mensaje.
    ImGui::Dummy(ImVec2(pad + 4.0f, 0.0f));
}

} // namespace

void LogPanel::push(const std::string& message)
{
    // Protocolo de módulo sobre el callback de un solo argumento que usan
    // todos los callers actuales (EditorContext::pushLog): un mensaje que
    // empieza por "[Modulo] " se etiqueta con ese módulo. Si no encaja el
    // patrón, la entrada cae en el módulo genérico y se pinta como siempre.
    if (!message.empty() && message.front() == '[') {
        const size_t close = message.find(']');
        if (close != std::string::npos && close > 1 && close <= 25 && close + 1 < message.size() &&
            message[close + 1] == ' ') {
            bool ok = true;
            for (size_t i = 1; i < close; ++i) {
                const char c = message[i];
                if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '.' && c != '-') {
                    ok = false;
                    break;
                }
            }
            if (ok) {
                push(message.substr(close + 2), message.substr(1, close - 1));
                return;
            }
        }
    }
    push(message, kDefaultModule);
}

void LogPanel::push(const std::string& message, const std::string& module)
{
    std::time_t t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm     tmBuf{};
    localtime_s(&tmBuf, &t);
    char timeStr[16];
    std::strftime(timeStr, sizeof(timeStr), "%H:%M:%S", &tmBuf);

    const std::string prefix = std::string("[") + timeStr + "] ";
    const std::string mod    = module.empty() ? kDefaultModule : module;

    // Una entrada por LÍNEA, no por mensaje. El panel pinta las filas con
    // ImGuiListClipper, que da por hecho que todas miden lo mismo: le basta
    // medir la primera visible para colocar las 200. Un mensaje con '\n'
    // dentro —los errores de Lua traen "stack traceback:" en varias líneas—
    // ocupa 3 renglones donde el clipper cuenta 1, así que el contenido real
    // acaba más abajo de donde el clipper cree que acaba. Lo que se veía:
    // SetScrollHereY(1.0f) apuntaba al fondo SEGÚN EL CLIPPER, 52 px por
    // encima del fondo de verdad (medido con dos errores de Lua en el log),
    // y el panel se subía solo cada vez que el usuario llegaba abajo con la
    // rueda o soltaba la barra de scroll.
    //
    // El troceado va aquí y no en el caller porque los callers son decenas
    // (EditorContext::pushLog, ScriptManager, los paneles) y ninguno sabe si
    // el texto que reenvía trae saltos dentro.
    //
    // Una línea vacía intermedia no genera fila: separa visualmente en un
    // terminal, en el panel solo sería un renglón en blanco. Un mensaje vacío
    // sí deja su fila, que es lo que hacía antes.
    size_t start   = 0;
    bool   anyLine = false;
    while (start <= message.size()) {
        size_t nl  = message.find('\n', start);
        size_t end = (nl == std::string::npos) ? message.size() : nl;
        // Un "\r\n" deja el retorno de carro pegado al final de la línea: sin
        // quitarlo se cuela en el portapapeles al copiar.
        size_t stop = end;
        if (stop > start && message[stop - 1] == '\r')
            --stop;

        if (stop > start) {
            Entry e;
            e.prefix  = prefix;
            e.message = message.substr(start, stop - start);
            e.module  = mod;
            e.id      = m_nextId++;
            m_entries.push_back(std::move(e));
            // El tope se aplica por FILA: un solo mensaje de 500 líneas no
            // puede saltárselo.
            if (m_entries.size() > kLogMaxEntries)
                m_entries.pop_front();
            anyLine = true;
        }

        if (nl == std::string::npos)
            break;
        start = nl + 1;
    }

    if (!anyLine) {
        Entry e;
        e.prefix = prefix;
        e.module = mod;
        e.id     = m_nextId++;
        m_entries.push_back(std::move(e));
        if (m_entries.size() > kLogMaxEntries)
            m_entries.pop_front();
    }
}

void LogPanel::handleRowClick(size_t index)
{
    const ImGuiIO& io = ImGui::GetIO();
    if (io.KeyShift && m_anchorId != 0) {
        size_t anchor = index;
        for (size_t i = 0; i < m_entries.size(); ++i) {
            if (m_entries[i].id == m_anchorId) {
                anchor = i;
                break;
            }
        }
        const size_t lo = anchor < index ? anchor : index;
        const size_t hi = anchor < index ? index : anchor;
        for (size_t i = 0; i < m_entries.size(); ++i)
            m_entries[i].selected = (i >= lo && i <= hi);
        return;  // el ancla no se mueve: permite reajustar el rango
    }
    if (io.KeyCtrl) {
        m_entries[index].selected = !m_entries[index].selected;
        m_anchorId                = m_entries[index].id;
        return;
    }
    for (auto& e : m_entries)
        e.selected = false;
    m_entries[index].selected = true;
    m_anchorId                = m_entries[index].id;
}

void LogPanel::copySelection()
{
    bool anySelected = false;
    for (const auto& e : m_entries) {
        if (e.selected) {
            anySelected = true;
            break;
        }
    }

    std::string out;
    for (const auto& e : m_entries) {
        if (anySelected && !e.selected)
            continue;
        out += e.prefix;
        if (e.module != kDefaultModule)
            out += "[" + e.module + "] ";
        out += e.message;
        out += '\n';
    }
    if (!out.empty())
        ImGui::SetClipboardText(out.c_str());
}

void LogPanel::drawRow(size_t index)
{
    Entry& e = m_entries[index];
    ImGui::PushID(static_cast<int>(index));

    // El Selectable ocupa la fila entera y va debajo del texto (AllowOverlap):
    // rebobinamos el cursor para pintar prefijo, chip y mensaje encima.
    const ImVec2 rowPos = ImGui::GetCursorPos();
    if (ImGui::Selectable("##logrow", e.selected, ImGuiSelectableFlags_AllowOverlap))
        handleRowClick(index);
    ImGui::SetCursorPos(rowPos);

    ImGui::TextUnformatted(e.prefix.c_str());
    ImGui::SameLine(0.0f, 0.0f);
    if (e.module != kDefaultModule) {
        drawModuleChip(e.module);
        ImGui::SameLine(0.0f, 0.0f);
    }
    ImGui::TextUnformatted(e.message.c_str());

    ImGui::PopID();
}

void LogPanel::draw()
{
    if (!m_open) return;
    ImGui::Begin("Log", &m_open);

    if (ImGui::Button("Copy"))
        copySelection();
    ImGui::SameLine();
    ImGui::TextDisabled("click / Ctrl+click / Shift+click, Ctrl+C copia");
    ImGui::Separator();

    // Clipper: solo se pintan las filas visibles, no las 200 del buffer.
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(m_entries.size()));
    while (clipper.Step()) {
        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
            drawRow(static_cast<size_t>(i));
    }
    clipper.End();

    // Autoscroll: solo si ya estaba al fondo antes de este frame (no pelea
    // con el usuario si sube a revisar historial mientras entran líneas
    // nuevas).
    if (m_autoScroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
        ImGui::SetScrollHereY(1.0f);
    m_autoScroll = ImGui::GetScrollY() >= ImGui::GetScrollMaxY();

    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && ImGui::GetIO().KeyCtrl &&
        ImGui::IsKeyPressed(ImGuiKey_C, false))
        copySelection();

    ImGui::End();
}

} // namespace DonTopo
