#include "DonTopo/Editor/InputActionsPanel.h"

#include "DonTopo/Core/Input.h"
#include "DonTopo/Scripting/LuaApiReference.h"

#include <GLFW/glfw3.h>
#include <imgui.h>
#include <nlohmann/json.hpp>

#include <cstring>
#include <fstream>
#include <utility>

namespace DonTopo {

namespace {
    // Junto al imgui.ini del editor (directorio de trabajo), que es donde el
    // editor ya deja su configuración de sesión: el mapa de acciones es
    // configuración del proyecto abierto, no un asset de la escena, así que no
    // va en assets/ ni dentro del .json de escena.
    const char* kInputActionsFile = "input_actions.json";

    // Longitud máxima de nombre, la de los buffers del panel.
    constexpr size_t kNameCap = 63;

    // Publica un snippet por acción y función en el autocomplete del Script
    // Editor. Con la lista vacía, el popup queda exactamente como antes.
    void publishAutocomplete(const std::vector<InputActionsPanel::Action>& actions)
    {
        std::vector<std::string> symbols;
        symbols.reserve(actions.size() * 3);
        for (const auto& a : actions)
        {
            symbols.push_back("Input.IsActionDown(\"" + a.name + "\")");
            symbols.push_back("Input.IsActionPressed(\"" + a.name + "\")");
            symbols.push_back("Input.IsActionReleased(\"" + a.name + "\")");
        }
        setLuaApiActionSymbols(std::move(symbols));
    }
}

// La traducción vive aquí y no en Core porque Core no puede incluir <imgui.h>
// (split Core/Editor): el editor traduce al guardar y Core lee ya traducido.
bool InputActionsPanel::bindingToGlfw(int imguiKey, const char*& outDevice, int& outCode)
{
    // Ratón: ImGui los mete en el mismo rango de ImGuiKey que las teclas.
    if (imguiKey >= ImGuiKey_MouseLeft && imguiKey <= ImGuiKey_MouseX2)
    {
        outDevice = "mouse";
        outCode   = imguiKey - ImGuiKey_MouseLeft;   // GLFW_MOUSE_BUTTON_LEFT == 0
        return true;
    }
    // Mando: solo los botones digitales tienen equivalente en GLFW. Los
    // gatillos analógicos (L2/R2) y los sticks son ejes, no botones: se
    // siguen viendo en el panel pero no llegan al mapa de runtime.
    if (imguiKey >= ImGuiKey_GamepadStart && imguiKey <= ImGuiKey_GamepadRStickDown)
    {
        outDevice = "pad";
        switch (imguiKey)
        {
            case ImGuiKey_GamepadStart:     outCode = GLFW_GAMEPAD_BUTTON_START;         return true;
            case ImGuiKey_GamepadBack:      outCode = GLFW_GAMEPAD_BUTTON_BACK;          return true;
            case ImGuiKey_GamepadFaceDown:  outCode = GLFW_GAMEPAD_BUTTON_A;             return true;
            case ImGuiKey_GamepadFaceRight: outCode = GLFW_GAMEPAD_BUTTON_B;             return true;
            case ImGuiKey_GamepadFaceLeft:  outCode = GLFW_GAMEPAD_BUTTON_X;             return true;
            case ImGuiKey_GamepadFaceUp:    outCode = GLFW_GAMEPAD_BUTTON_Y;             return true;
            case ImGuiKey_GamepadDpadLeft:  outCode = GLFW_GAMEPAD_BUTTON_DPAD_LEFT;     return true;
            case ImGuiKey_GamepadDpadRight: outCode = GLFW_GAMEPAD_BUTTON_DPAD_RIGHT;    return true;
            case ImGuiKey_GamepadDpadUp:    outCode = GLFW_GAMEPAD_BUTTON_DPAD_UP;       return true;
            case ImGuiKey_GamepadDpadDown:  outCode = GLFW_GAMEPAD_BUTTON_DPAD_DOWN;     return true;
            case ImGuiKey_GamepadL1:        outCode = GLFW_GAMEPAD_BUTTON_LEFT_BUMPER;   return true;
            case ImGuiKey_GamepadR1:        outCode = GLFW_GAMEPAD_BUTTON_RIGHT_BUMPER;  return true;
            case ImGuiKey_GamepadL3:        outCode = GLFW_GAMEPAD_BUTTON_LEFT_THUMB;    return true;
            case ImGuiKey_GamepadR3:        outCode = GLFW_GAMEPAD_BUTTON_RIGHT_THUMB;   return true;
            default: return false;   // L2/R2 y sticks: ejes, no botones
        }
    }

    outDevice = "key";
    // Rangos contiguos en ambos enums.
    if (imguiKey >= ImGuiKey_0 && imguiKey <= ImGuiKey_9)
    { outCode = GLFW_KEY_0 + (imguiKey - ImGuiKey_0); return true; }
    if (imguiKey >= ImGuiKey_A && imguiKey <= ImGuiKey_Z)
    { outCode = GLFW_KEY_A + (imguiKey - ImGuiKey_A); return true; }
    if (imguiKey >= ImGuiKey_F1 && imguiKey <= ImGuiKey_F12)
    { outCode = GLFW_KEY_F1 + (imguiKey - ImGuiKey_F1); return true; }
    if (imguiKey >= ImGuiKey_Keypad0 && imguiKey <= ImGuiKey_Keypad9)
    { outCode = GLFW_KEY_KP_0 + (imguiKey - ImGuiKey_Keypad0); return true; }

    switch (imguiKey)
    {
        case ImGuiKey_Tab:          outCode = GLFW_KEY_TAB;            return true;
        case ImGuiKey_LeftArrow:    outCode = GLFW_KEY_LEFT;           return true;
        case ImGuiKey_RightArrow:   outCode = GLFW_KEY_RIGHT;          return true;
        case ImGuiKey_UpArrow:      outCode = GLFW_KEY_UP;             return true;
        case ImGuiKey_DownArrow:    outCode = GLFW_KEY_DOWN;           return true;
        case ImGuiKey_PageUp:       outCode = GLFW_KEY_PAGE_UP;        return true;
        case ImGuiKey_PageDown:     outCode = GLFW_KEY_PAGE_DOWN;      return true;
        case ImGuiKey_Home:         outCode = GLFW_KEY_HOME;           return true;
        case ImGuiKey_End:          outCode = GLFW_KEY_END;            return true;
        case ImGuiKey_Insert:       outCode = GLFW_KEY_INSERT;         return true;
        case ImGuiKey_Delete:       outCode = GLFW_KEY_DELETE;         return true;
        case ImGuiKey_Backspace:    outCode = GLFW_KEY_BACKSPACE;      return true;
        case ImGuiKey_Space:        outCode = GLFW_KEY_SPACE;          return true;
        case ImGuiKey_Enter:        outCode = GLFW_KEY_ENTER;          return true;
        case ImGuiKey_Escape:       outCode = GLFW_KEY_ESCAPE;         return true;
        case ImGuiKey_LeftCtrl:     outCode = GLFW_KEY_LEFT_CONTROL;   return true;
        case ImGuiKey_LeftShift:    outCode = GLFW_KEY_LEFT_SHIFT;     return true;
        case ImGuiKey_LeftAlt:      outCode = GLFW_KEY_LEFT_ALT;       return true;
        case ImGuiKey_LeftSuper:    outCode = GLFW_KEY_LEFT_SUPER;     return true;
        case ImGuiKey_RightCtrl:    outCode = GLFW_KEY_RIGHT_CONTROL;  return true;
        case ImGuiKey_RightShift:   outCode = GLFW_KEY_RIGHT_SHIFT;    return true;
        case ImGuiKey_RightAlt:     outCode = GLFW_KEY_RIGHT_ALT;      return true;
        case ImGuiKey_RightSuper:   outCode = GLFW_KEY_RIGHT_SUPER;    return true;
        case ImGuiKey_Menu:         outCode = GLFW_KEY_MENU;           return true;
        case ImGuiKey_Apostrophe:   outCode = GLFW_KEY_APOSTROPHE;     return true;
        case ImGuiKey_Comma:        outCode = GLFW_KEY_COMMA;          return true;
        case ImGuiKey_Minus:        outCode = GLFW_KEY_MINUS;          return true;
        case ImGuiKey_Period:       outCode = GLFW_KEY_PERIOD;         return true;
        case ImGuiKey_Slash:        outCode = GLFW_KEY_SLASH;          return true;
        case ImGuiKey_Semicolon:    outCode = GLFW_KEY_SEMICOLON;      return true;
        case ImGuiKey_Equal:        outCode = GLFW_KEY_EQUAL;          return true;
        case ImGuiKey_LeftBracket:  outCode = GLFW_KEY_LEFT_BRACKET;   return true;
        case ImGuiKey_Backslash:    outCode = GLFW_KEY_BACKSLASH;      return true;
        case ImGuiKey_RightBracket: outCode = GLFW_KEY_RIGHT_BRACKET;  return true;
        case ImGuiKey_GraveAccent:  outCode = GLFW_KEY_GRAVE_ACCENT;   return true;
        case ImGuiKey_CapsLock:     outCode = GLFW_KEY_CAPS_LOCK;      return true;
        case ImGuiKey_ScrollLock:   outCode = GLFW_KEY_SCROLL_LOCK;    return true;
        case ImGuiKey_NumLock:      outCode = GLFW_KEY_NUM_LOCK;       return true;
        case ImGuiKey_PrintScreen:  outCode = GLFW_KEY_PRINT_SCREEN;   return true;
        case ImGuiKey_Pause:        outCode = GLFW_KEY_PAUSE;          return true;
        case ImGuiKey_KeypadDecimal:  outCode = GLFW_KEY_KP_DECIMAL;   return true;
        case ImGuiKey_KeypadDivide:   outCode = GLFW_KEY_KP_DIVIDE;    return true;
        case ImGuiKey_KeypadMultiply: outCode = GLFW_KEY_KP_MULTIPLY;  return true;
        case ImGuiKey_KeypadSubtract: outCode = GLFW_KEY_KP_SUBTRACT;  return true;
        case ImGuiKey_KeypadAdd:      outCode = GLFW_KEY_KP_ADD;       return true;
        case ImGuiKey_KeypadEnter:    outCode = GLFW_KEY_KP_ENTER;     return true;
        case ImGuiKey_KeypadEqual:    outCode = GLFW_KEY_KP_EQUAL;     return true;
        default: return false;
    }
}

int InputActionsPanel::padButtonToBinding(int glfwButton)
{
    switch (glfwButton)
    {
        case GLFW_GAMEPAD_BUTTON_A:            return ImGuiKey_GamepadFaceDown;
        case GLFW_GAMEPAD_BUTTON_B:            return ImGuiKey_GamepadFaceRight;
        case GLFW_GAMEPAD_BUTTON_X:            return ImGuiKey_GamepadFaceLeft;
        case GLFW_GAMEPAD_BUTTON_Y:            return ImGuiKey_GamepadFaceUp;
        case GLFW_GAMEPAD_BUTTON_LEFT_BUMPER:  return ImGuiKey_GamepadL1;
        case GLFW_GAMEPAD_BUTTON_RIGHT_BUMPER: return ImGuiKey_GamepadR1;
        case GLFW_GAMEPAD_BUTTON_BACK:         return ImGuiKey_GamepadBack;
        case GLFW_GAMEPAD_BUTTON_START:        return ImGuiKey_GamepadStart;
        // GLFW_GAMEPAD_BUTTON_GUIDE (el botón de Xbox/PS del centro): ImGui no
        // tiene ImGuiKey para él, así que no se puede bindear.
        case GLFW_GAMEPAD_BUTTON_LEFT_THUMB:   return ImGuiKey_GamepadL3;
        case GLFW_GAMEPAD_BUTTON_RIGHT_THUMB:  return ImGuiKey_GamepadR3;
        case GLFW_GAMEPAD_BUTTON_DPAD_UP:      return ImGuiKey_GamepadDpadUp;
        case GLFW_GAMEPAD_BUTTON_DPAD_RIGHT:   return ImGuiKey_GamepadDpadRight;
        case GLFW_GAMEPAD_BUTTON_DPAD_DOWN:    return ImGuiKey_GamepadDpadDown;
        case GLFW_GAMEPAD_BUTTON_DPAD_LEFT:    return ImGuiKey_GamepadDpadLeft;
        default: return -1;   // fuera de rango, o botón sin ImGuiKey
    }
}

InputActionsPanel::InputActionsPanel()
{
    load();
    // Fichero anterior a las acciones en scripting (solo "bindings"): se
    // reescribe una vez con la traducción a GLFW ya dentro. save() se encarga
    // también de publicar el autocomplete y de refrescar el mapa de Core.
    if (m_needsMigrationSave)
        save();
    else
        publishAutocomplete(m_actions);
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
        // El panel no lee "glfw" (su modelo son ImGuiKey y de ahí se regenera):
        // solo mira si falta, para reescribir el fichero migrado una vez.
        if (aj.find("glfw") == aj.end())
            m_needsMigrationSave = true;
        m_actions.push_back(std::move(a));
    }
    return true;
}

void InputActionsPanel::save() const
{
    nlohmann::json actions = nlohmann::json::array();
    for (const Action& a : m_actions)
    {
        // "bindings" (ImGuiKey) se sigue escribiendo igual —es lo que pinta la
        // UI y lo que lee la versión anterior del panel— y "glfw" se añade con
        // la traducción que Core sí entiende.
        nlohmann::json glfw = nlohmann::json::array();
        for (int b : a.bindings)
        {
            const char* device = nullptr;
            int code = 0;
            if (bindingToGlfw(b, device, code))
                glfw.push_back({ {"device", device}, {"code", code} });
        }
        actions.push_back({ {"name", a.name}, {"bindings", a.bindings}, {"glfw", std::move(glfw)} });
    }

    {
        std::ofstream file(kInputActionsFile);
        if (!file.is_open()) return;   // disco de solo lectura: se pierde el guardado, no el editor
        file << nlohmann::json{ {"actions", std::move(actions)} }.dump(2);
    }   // cerrado antes de que Core lo relea

    // Core relee el mapa y el Script Editor recibe los snippets: una acción
    // recién creada vale en el siguiente Play y se autocompleta sin reiniciar.
    Input::reloadActions();
    publishAutocomplete(m_actions);
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

int InputActionsPanel::pollFirstPressedPadButton() const
{
    // Core ya sondea el mando una vez por frame (Input::update, fuera del gate
    // de Play) y guarda prev/curr, así que aquí hay flanco de bajada sin estado
    // propio: mantener pulsado un botón no encadena bindings.
    for (int b = 0; b <= GLFW_GAMEPAD_BUTTON_LAST; ++b)
    {
        if (!Input::isPadButtonPressed(b)) continue;
        const int key = padButtonToBinding(b);
        if (key >= 0) return key;   // GUIDE no tiene ImGuiKey: se ignora
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
                // Teclado/ratón por ImGui; mando por Core. El orden importa
                // poco (no se puede pulsar tecla y botón el mismo frame), pero
                // el mando va después porque es el caso raro.
                int key = pollFirstPressedKey();
                if (key < 0) key = pollFirstPressedPadButton();
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
                    // Sin mando reconocido no llega ningun boton: decirlo aqui
                    // evita que parezca que la escucha esta rota.
                    if (!glfwJoystickIsGamepad(GLFW_JOYSTICK_1))
                    {
                        ImGui::SameLine();
                        ImGui::TextDisabled("[sin mando]");
                    }
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
