#include "DonTopo/Editor/InputActionsPanel.h"

#include <imgui.h>
#include <nlohmann/json.hpp>

#include <cstring>
#include <fstream>

namespace DonTopo {

namespace {
    // Junto al imgui.ini del editor (directorio de trabajo), que es donde el
    // editor ya deja su configuración de sesión: el mapa de acciones es
    // configuración del proyecto abierto, no un asset de la escena, así que no
    // va en assets/ ni dentro del .json de escena.
    const char* kInputActionsFile = "input_actions.json";

    // Longitud máxima de nombre, la de los buffers del panel.
    constexpr size_t kNameCap = 63;
}

InputActionsPanel::InputActionsPanel()
{
    load();
}

int InputActionsPanel::findAction(const std::string& name) const
{
    for (size_t i = 0; i < m_actions.size(); ++i)
        if (m_actions[i].name == name) return static_cast<int>(i);
    return -1;
}

bool InputActionsPanel::load()
{
    std::ifstream file(kInputActionsFile);
    if (!file.is_open()) return false;

    // Un JSON roto no puede tumbar el arranque del editor: se ignora y el panel
    // queda vacío. El primer save() lo reescribe entero.
    nlohmann::json j = nlohmann::json::parse(file, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return false;

    auto it = j.find("actions");
    if (it == j.end() || !it->is_array()) return false;

    m_actions.clear();
    for (const auto& aj : *it)
    {
        if (!aj.is_object()) continue;
        auto nameIt = aj.find("name");
        if (nameIt == aj.end() || !nameIt->is_string()) continue;

        Action a;
        a.name = nameIt->get<std::string>();
        if (a.name.empty() || findAction(a.name) >= 0) continue;   // duplicado en disco: se queda el primero

        auto bindIt = aj.find("bindings");
        if (bindIt != aj.end() && bindIt->is_array())
        {
            for (const auto& bj : *bindIt)
            {
                if (!bj.is_number_integer()) continue;
                const int key = bj.get<int>();
                // Un valor fuera del rango de ImGuiKey (fichero de otra versión,
                // edición a mano) rompería GetKeyName al dibujar.
                if (key < ImGuiKey_NamedKey_BEGIN || key >= ImGuiKey_NamedKey_END) continue;
                a.bindings.push_back(key);
            }
        }
        m_actions.push_back(std::move(a));
    }
    return true;
}

void InputActionsPanel::save() const
{
    nlohmann::json actions = nlohmann::json::array();
    for (const Action& a : m_actions)
        actions.push_back({ {"name", a.name}, {"bindings", a.bindings} });

    std::ofstream file(kInputActionsFile);
    if (!file.is_open()) return;   // disco de solo lectura: se pierde el guardado, no el editor
    file << nlohmann::json{ {"actions", std::move(actions)} }.dump(2);
}

int InputActionsPanel::pollFirstPressedKey() const
{
    for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END; ++k)
    {
        const ImGuiKey key = static_cast<ImGuiKey>(k);
        if (key == ImGuiKey_Escape) continue;   // reservada para cancelar la escucha
        // Los huecos reservados del enum (ImGuiKey_ReservedForMod*) no tienen
        // nombre: bindearlos daría una fila sin etiqueta.
        const char* name = ImGui::GetKeyName(key);
        if (!name || name[0] == '\0' || std::strcmp(name, "Unknown") == 0) continue;
        // repeat=false: una pulsación mantenida no debe encadenar bindings.
        if (ImGui::IsKeyPressed(key, false)) return k;
    }
    return -1;
}

void InputActionsPanel::draw()
{
    // Coste cero con el panel cerrado, incluida la escucha: si el panel se
    // cierra en mitad de un "Add Binding", la escucha muere con él y al reabrir
    // el panel no queda ningún frame capturando teclas a espaldas del usuario.
    if (!m_open) return;

    if (ImGui::Begin("Input Actions", &m_open))
    {
        // --- Barra de creación ---
        ImGui::SetNextItemWidth(200.0f);
        const bool submitted = ImGui::InputText("##newActionName", m_newNameBuf, sizeof(m_newNameBuf),
                                                ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine();
        if (ImGui::Button("Create") || submitted)
        {
            const std::string name(m_newNameBuf);
            if (name.empty())
            {
                m_warning = "El nombre no puede estar vacio.";
            }
            else if (findAction(name) >= 0)
            {
                m_warning = "Ya existe una accion llamada '" + name + "'.";
            }
            else
            {
                m_actions.push_back(Action{ name, {} });
                m_newNameBuf[0] = '\0';
                m_warning.clear();
                save();
            }
        }

        if (!m_warning.empty())
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", m_warning.c_str());

        ImGui::Separator();

        if (m_actions.empty())
        {
            ImGui::TextDisabled("No hay acciones. Escribe un nombre y pulsa Create.");
        }

        // --- Escucha de binding ---
        // Se resuelve antes de dibujar la lista para que el binding capturado
        // aparezca ya en este mismo frame.
        if (m_listeningIndex >= 0)
        {
            if (m_listeningIndex >= static_cast<int>(m_actions.size()))
            {
                m_listeningIndex = -1;   // la acción se borró mientras escuchaba
            }
            else if (ImGui::IsKeyPressed(ImGuiKey_Escape, false))
            {
                m_listeningIndex = -1;
            }
            else
            {
                const int key = pollFirstPressedKey();
                if (key >= 0)
                {
                    Action& a = m_actions[m_listeningIndex];
                    // Un binding repetido en la misma acción no aporta nada y
                    // duplicaría la fila.
                    bool already = false;
                    for (int b : a.bindings) if (b == key) { already = true; break; }
                    if (!already) a.bindings.push_back(key);
                    m_listeningIndex = -1;
                    save();
                }
            }
        }

        // Índices de la acción a borrar y del binding a quitar. Diferidos: no se
        // puede mutar el vector mientras se recorre dibujando.
        int deleteAction  = -1;
        int removeFromAct = -1;
        int removeBinding = -1;

        for (int i = 0; i < static_cast<int>(m_actions.size()); ++i)
        {
            Action& a = m_actions[i];
            ImGui::PushID(i);

            if (m_renamingIndex == i)
            {
                ImGui::SetNextItemWidth(200.0f);
                const bool renameSubmitted = ImGui::InputText("##renameBuf", m_renameBuf, sizeof(m_renameBuf),
                                                              ImGuiInputTextFlags_EnterReturnsTrue);
                ImGui::SameLine();
                if (ImGui::Button("OK") || renameSubmitted)
                {
                    const std::string name(m_renameBuf);
                    const int clash = findAction(name);
                    if (name.empty())
                    {
                        m_warning = "El nombre no puede estar vacio.";
                    }
                    else if (clash >= 0 && clash != i)
                    {
                        m_warning = "Ya existe una accion llamada '" + name + "'.";
                    }
                    else
                    {
                        a.name = name;
                        m_renamingIndex = -1;
                        m_warning.clear();
                        save();
                    }
                }
                ImGui::SameLine();
                if (ImGui::Button("Cancel")) { m_renamingIndex = -1; m_warning.clear(); }
            }
            else
            {
                ImGui::Text("%s", a.name.c_str());
                ImGui::SameLine();
                if (ImGui::Button("Rename"))
                {
                    m_renamingIndex = i;
                    std::strncpy(m_renameBuf, a.name.c_str(), kNameCap);
                    m_renameBuf[kNameCap] = '\0';
                    m_warning.clear();
                }
                ImGui::SameLine();
                if (ImGui::Button("Delete")) deleteAction = i;
                ImGui::SameLine();
                if (m_listeningIndex == i)
                {
                    if (ImGui::Button("Cancel listen")) m_listeningIndex = -1;
                    ImGui::SameLine();
                    ImGui::TextDisabled("Pulsa una tecla, boton de raton o de mando (Esc cancela)");
                }
                else if (ImGui::Button("Add Binding"))
                {
                    m_listeningIndex = i;
                    m_warning.clear();
                }
            }

            ImGui::Indent();
            if (a.bindings.empty())
            {
                ImGui::TextDisabled("(sin bindings)");
            }
            for (int b = 0; b < static_cast<int>(a.bindings.size()); ++b)
            {
                ImGui::PushID(b);
                ImGui::Text("%s", ImGui::GetKeyName(static_cast<ImGuiKey>(a.bindings[b])));
                ImGui::SameLine();
                if (ImGui::Button("X")) { removeFromAct = i; removeBinding = b; }
                ImGui::PopID();
            }
            ImGui::Unindent();

            ImGui::PopID();
        }

        if (removeFromAct >= 0)
        {
            auto& bindings = m_actions[removeFromAct].bindings;
            bindings.erase(bindings.begin() + removeBinding);
            save();
        }
        if (deleteAction >= 0)
        {
            m_actions.erase(m_actions.begin() + deleteAction);
            // Los índices diferidos apuntan a posiciones que acaban de moverse.
            if (m_renamingIndex  == deleteAction) m_renamingIndex  = -1;
            if (m_listeningIndex == deleteAction) m_listeningIndex = -1;
            if (m_renamingIndex  > deleteAction) --m_renamingIndex;
            if (m_listeningIndex > deleteAction) --m_listeningIndex;
            save();
        }
    }
    ImGui::End();
}

} // namespace DonTopo
