#include "DonTopo/Editor/LogPanel.h"
#include <imgui.h>
#include <chrono>
#include <ctime>

namespace DonTopo {

void LogPanel::push(const std::string& message)
{
    std::time_t t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm     tmBuf{};
    localtime_s(&tmBuf, &t);
    char timeStr[16];
    std::strftime(timeStr, sizeof(timeStr), "%H:%M:%S", &tmBuf);

    m_entries.push_back(std::string("[") + timeStr + "] " + message);
    if (m_entries.size() > kLogMaxEntries)
        m_entries.pop_front();
}

void LogPanel::draw()
{
    if (!m_open) return;
    ImGui::Begin("Log", &m_open);
    for (const auto& line : m_entries)
        ImGui::TextUnformatted(line.c_str());

    // Autoscroll: solo si ya estaba al fondo antes de este frame (no pelea
    // con el usuario si sube a revisar historial mientras entran líneas
    // nuevas).
    if (m_autoScroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
        ImGui::SetScrollHereY(1.0f);
    m_autoScroll = ImGui::GetScrollY() >= ImGui::GetScrollMaxY();

    ImGui::End();
}

} // namespace DonTopo
