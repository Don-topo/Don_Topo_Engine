#include "DonTopo/Editor/LogPanel.h"
#include "DonTopo/Core/Platform.h"
#include <imgui.h>
#include <cctype>
#include <chrono>
#include <ctime>

namespace DonTopo {

namespace {

// Chip color from a stable hash (FNV-1a) of the module name:
// same module = same color across runs, and a new module does not force
// touching any table. Only the name is hashed to pick the hue; the
// saturation and value are fixed so that all chips are legible.
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

// Black text on light chips, white on dark ones.
ImVec4 chipTextColor(const ImVec4& bg)
{
    const float luma = 0.299f * bg.x + 0.587f * bg.y + 0.114f * bg.z;
    return luma > 0.6f ? ImVec4(0.05f, 0.05f, 0.05f, 1.0f) : ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
}

// Module chip: rounded rectangle with the name inside. Leaves the cursor
// ready for the caller to continue with SameLine(0, 0).
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
    // Right padding of the chip + gap to the message.
    ImGui::Dummy(ImVec2(pad + 4.0f, 0.0f));
}

} // namespace

void LogPanel::push(const std::string& message)
{
    // Module protocol over the single-argument callback that all current
    // callers use (EditorContext::pushLog): a message that
    // starts with "[Module] " is tagged with that module. If the pattern does not
    // match, the entry falls into the generic module and is drawn as always.
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
    const std::tm tmBuf = platform::localTime(t);
    char timeStr[16];
    std::strftime(timeStr, sizeof(timeStr), "%H:%M:%S", &tmBuf);

    const std::string prefix = std::string("[") + timeStr + "] ";
    const std::string mod    = module.empty() ? kDefaultModule : module;

    // One entry per LINE, not per message. The panel draws the rows with
    // ImGuiListClipper, which assumes they all measure the same: it is enough for it to
    // measure the first visible one to place the 200. A message with a '\n'
    // inside (Lua errors bring "stack traceback:" over several lines)
    // takes 3 lines where the clipper counts 1, so the real content
    // ends up lower than where the clipper thinks it ends. What was seen:
    // SetScrollHereY(1.0f) pointed to the bottom ACCORDING TO THE CLIPPER, 52 px
    // above the real bottom (measured with two Lua errors in the log),
    // and the panel scrolled up by itself every time the user reached the bottom with the
    // wheel or released the scroll bar.
    //
    // The splitting goes here and not in the caller because the callers are dozens
    // (EditorContext::pushLog, ScriptManager, the panels) and none knows whether
    // the text it forwards carries line breaks inside.
    //
    // An empty line in the middle does not generate a row: it separates visually in a
    // terminal, in the panel it would just be a blank line. An empty message
    // does keep its row, which is what it did before.
    size_t start   = 0;
    bool   anyLine = false;
    while (start <= message.size()) {
        size_t nl  = message.find('\n', start);
        size_t end = (nl == std::string::npos) ? message.size() : nl;
        // A "\r\n" leaves the carriage return stuck to the end of the line: without
        // removing it, it leaks into the clipboard when copying.
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
            // The cap is applied per ROW: a single message of 500 lines cannot
            // skip it.
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
        return;  // the anchor does not move: it allows adjusting the range
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

    // The Selectable takes the whole row and goes under the text (AllowOverlap):
    // we rewind the cursor to draw prefix, chip and message on top.
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
    ImGui::TextDisabled("click / Ctrl+click / Shift+click, Ctrl+C copies");
    ImGui::Separator();

    // Clipper: only the visible rows are drawn, not the 200 of the buffer.
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(m_entries.size()));
    while (clipper.Step()) {
        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
            drawRow(static_cast<size_t>(i));
    }
    clipper.End();

    // Autoscroll: only if it was already at the bottom before this frame (it does not fight
    // the user if they scroll up to review history while new lines
    // come in).
    if (m_autoScroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
        ImGui::SetScrollHereY(1.0f);
    m_autoScroll = ImGui::GetScrollY() >= ImGui::GetScrollMaxY();

    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && ImGui::GetIO().KeyCtrl &&
        ImGui::IsKeyPressed(ImGuiKey_C, false))
        copySelection();

    ImGui::End();
}

} // namespace DonTopo
