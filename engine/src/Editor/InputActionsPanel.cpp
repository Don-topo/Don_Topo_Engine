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
    // Next to the editor's imgui.ini (working directory), which is where the
    // editor already leaves its session configuration: the action map is
    // configuration of the open project, not a scene asset, so it does not
    // go in assets/ or inside the scene .json.
    const char* kInputActionsFile = "input_actions.json";

    // Maximum name length, that of the panel's buffers.
    constexpr size_t kNameCap = 63;

    // Publishes one snippet per action and function to the Script Editor's
    // autocomplete. With the list empty, the popup stays exactly as before.
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

// The translation lives here and not in Core because Core cannot include <imgui.h>
// (Core/Editor split): the editor translates on save and Core reads it already translated.
bool InputActionsPanel::bindingToGlfw(int imguiKey, const char*& outDevice, int& outCode)
{
    // Mouse: ImGui puts them in the same ImGuiKey range as the keys.
    if (imguiKey >= ImGuiKey_MouseLeft && imguiKey <= ImGuiKey_MouseX2)
    {
        outDevice = "mouse";
        outCode   = imguiKey - ImGuiKey_MouseLeft;   // GLFW_MOUSE_BUTTON_LEFT == 0
        return true;
    }
    // Gamepad. Digital buttons go to "pad" with their GLFW_GAMEPAD_BUTTON_*;
    // the triggers (L2/R2) and the stick directions are axes in GLFW and go to
    // "padaxis" with the axis+sign code of Input::padAxisCode. Careful with the sign of the
    // Y axis: in GLFW up is NEGATIVE.
    if (imguiKey >= ImGuiKey_GamepadStart && imguiKey <= ImGuiKey_GamepadRStickDown)
    {
        outDevice = "padaxis";
        switch (imguiKey)
        {
            case ImGuiKey_GamepadL2:
                outCode = Input::padAxisCode(GLFW_GAMEPAD_AXIS_LEFT_TRIGGER,  false); return true;
            case ImGuiKey_GamepadR2:
                outCode = Input::padAxisCode(GLFW_GAMEPAD_AXIS_RIGHT_TRIGGER, false); return true;
            case ImGuiKey_GamepadLStickRight:
                outCode = Input::padAxisCode(GLFW_GAMEPAD_AXIS_LEFT_X,  false); return true;
            case ImGuiKey_GamepadLStickLeft:
                outCode = Input::padAxisCode(GLFW_GAMEPAD_AXIS_LEFT_X,  true);  return true;
            case ImGuiKey_GamepadLStickDown:
                outCode = Input::padAxisCode(GLFW_GAMEPAD_AXIS_LEFT_Y,  false); return true;
            case ImGuiKey_GamepadLStickUp:
                outCode = Input::padAxisCode(GLFW_GAMEPAD_AXIS_LEFT_Y,  true);  return true;
            case ImGuiKey_GamepadRStickRight:
                outCode = Input::padAxisCode(GLFW_GAMEPAD_AXIS_RIGHT_X, false); return true;
            case ImGuiKey_GamepadRStickLeft:
                outCode = Input::padAxisCode(GLFW_GAMEPAD_AXIS_RIGHT_X, true);  return true;
            case ImGuiKey_GamepadRStickDown:
                outCode = Input::padAxisCode(GLFW_GAMEPAD_AXIS_RIGHT_Y, false); return true;
            case ImGuiKey_GamepadRStickUp:
                outCode = Input::padAxisCode(GLFW_GAMEPAD_AXIS_RIGHT_Y, true);  return true;
            default: break;   // the rest are digital buttons
        }

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
            default: return false;   // gap in ImGui's enum with no equivalent
        }
    }

    outDevice = "key";
    // Contiguous ranges in both enums.
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
        // GLFW_GAMEPAD_BUTTON_GUIDE (the Xbox/PS button in the center): ImGui does not
        // have an ImGuiKey for it, so it cannot be bound.
        case GLFW_GAMEPAD_BUTTON_LEFT_THUMB:   return ImGuiKey_GamepadL3;
        case GLFW_GAMEPAD_BUTTON_RIGHT_THUMB:  return ImGuiKey_GamepadR3;
        case GLFW_GAMEPAD_BUTTON_DPAD_UP:      return ImGuiKey_GamepadDpadUp;
        case GLFW_GAMEPAD_BUTTON_DPAD_RIGHT:   return ImGuiKey_GamepadDpadRight;
        case GLFW_GAMEPAD_BUTTON_DPAD_DOWN:    return ImGuiKey_GamepadDpadDown;
        case GLFW_GAMEPAD_BUTTON_DPAD_LEFT:    return ImGuiKey_GamepadDpadLeft;
        default: return -1;   // out of range, or button without an ImGuiKey
    }
}

int InputActionsPanel::padAxisToBinding(int code)
{
    if (code < 0 || code >= Input::kPadAxisBindingCount) return -1;

    const int  axis     = Input::padAxisIndex(code);
    const bool negative = Input::padAxisNegative(code);
    switch (axis)
    {
        case GLFW_GAMEPAD_AXIS_LEFT_X:
            return negative ? ImGuiKey_GamepadLStickLeft : ImGuiKey_GamepadLStickRight;
        case GLFW_GAMEPAD_AXIS_LEFT_Y:
            return negative ? ImGuiKey_GamepadLStickUp   : ImGuiKey_GamepadLStickDown;
        case GLFW_GAMEPAD_AXIS_RIGHT_X:
            return negative ? ImGuiKey_GamepadRStickLeft : ImGuiKey_GamepadRStickRight;
        case GLFW_GAMEPAD_AXIS_RIGHT_Y:
            return negative ? ImGuiKey_GamepadRStickUp   : ImGuiKey_GamepadRStickDown;
        // A trigger is only pressed in one direction: its negative direction is not
        // a binding.
        case GLFW_GAMEPAD_AXIS_LEFT_TRIGGER:
            return negative ? -1 : ImGuiKey_GamepadL2;
        case GLFW_GAMEPAD_AXIS_RIGHT_TRIGGER:
            return negative ? -1 : ImGuiKey_GamepadR2;
        default: return -1;
    }
}

InputActionsPanel::InputActionsPanel()
{
    load();
    // File older than the actions in scripting (only "bindings"): it is
    // rewritten once with the GLFW translation already in. save() also takes care
    // of publishing the autocomplete and refreshing Core's map.
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

    // A broken JSON cannot take down the editor startup: it is ignored and the panel
    // stays empty. The first save() rewrites it whole.
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
        if (a.name.empty() || findAction(a.name) >= 0) continue;   // duplicate on disk: the first one stays

        auto bindIt = aj.find("bindings");
        if (bindIt != aj.end() && bindIt->is_array())
        {
            for (const auto& bj : *bindIt)
            {
                if (!bj.is_number_integer()) continue;
                const int key = bj.get<int>();
                // A value outside the ImGuiKey range (file from another version,
                // manual edit) would break GetKeyName when drawing.
                if (key < ImGuiKey_NamedKey_BEGIN || key >= ImGuiKey_NamedKey_END) continue;
                a.bindings.push_back(key);
            }
        }
        // The panel does not read "glfw" (its model is ImGuiKey and it is regenerated from that):
        // it only checks whether what is on disk matches what the translation of
        // NOW would produce. Checking whether the array is missing is not enough: a file
        // saved by a version with fewer translatable devices (the
        // sticks did not reach the runtime) carries "glfw" but incomplete, and those
        // bindings would stay drawn and dead until the panel is touched.
        size_t translatable = 0;
        for (int b : a.bindings)
        {
            const char* device = nullptr;
            int code = 0;
            if (bindingToGlfw(b, device, code)) ++translatable;
        }
        auto glfwIt = aj.find("glfw");
        if (glfwIt == aj.end() || !glfwIt->is_array() || glfwIt->size() != translatable)
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
        // "bindings" (ImGuiKey) is still written the same way (it is what the
        // UI draws and what the previous version of the panel reads) and "glfw" is added with
        // the translation that Core does understand.
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
        if (!file.is_open()) return;   // read-only disk: the save is lost, not the editor
        file << nlohmann::json{ {"actions", std::move(actions)} }.dump(2);
    }   // closed before Core re-reads it

    // Core re-reads the map and the Script Editor receives the snippets: a freshly
    // created action works in the next Play and autocompletes without restarting.
    Input::reloadActions();
    publishAutocomplete(m_actions);
}

int InputActionsPanel::pollFirstPressedKey() const
{
    for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END; ++k)
    {
        const ImGuiKey key = static_cast<ImGuiKey>(k);
        if (key == ImGuiKey_Escape) continue;   // reserved to cancel the listening
        // The reserved gaps of the enum (ImGuiKey_ReservedForMod*) have no
        // name: binding them would give a row without a label.
        const char* name = ImGui::GetKeyName(key);
        if (!name || name[0] == '\0' || std::strcmp(name, "Unknown") == 0) continue;
        // repeat=false: a held press must not chain bindings.
        if (ImGui::IsKeyPressed(key, false)) return k;
    }
    return -1;
}

int InputActionsPanel::pollFirstPressedPadButton() const
{
    // Core already polls the gamepad once per frame (Input::update, outside the Play
    // gate) and stores prev/curr, so here there is a falling edge without state
    // of its own: holding a button down does not chain bindings.
    for (int b = 0; b <= GLFW_GAMEPAD_BUTTON_LAST; ++b)
    {
        if (!Input::isPadButtonPressed(b)) continue;
        const int key = padButtonToBinding(b);
        if (key >= 0) return key;   // GUIDE has no ImGuiKey: ignored
    }
    return -1;
}

int InputActionsPanel::pollFirstPressedPadAxis() const
{
    for (int c = 0; c < Input::kPadAxisBindingCount; ++c)
    {
        if (!Input::isPadAxisPressed(c)) continue;
        const int key = padAxisToBinding(c);
        if (key >= 0) return key;   // trigger in the negative: not bindable
    }
    return -1;
}

void InputActionsPanel::draw()
{
    // Zero cost with the panel closed, listening included: if the panel is
    // closed in the middle of an "Add Binding", the listening dies with it and on reopening
    // the panel there is no frame capturing keys behind the user's back.
    if (!m_open) return;

    if (ImGui::Begin("Input Actions", &m_open))
    {
        // --- Creation bar ---
        ImGui::SetNextItemWidth(200.0f);
        const bool submitted = ImGui::InputText("##newActionName", m_newNameBuf, sizeof(m_newNameBuf),
                                                ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine();
        if (ImGui::Button("Create") || submitted)
        {
            const std::string name(m_newNameBuf);
            if (name.empty())
            {
                m_warning = "The name cannot be empty.";
            }
            else if (findAction(name) >= 0)
            {
                m_warning = "There is already an action named '" + name + "'.";
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
            ImGui::TextDisabled("No actions. Type a name and press Create.");
        }

        // --- Binding listening ---
        // It is resolved before drawing the list so that the captured binding
        // already appears in this same frame.
        if (m_listeningIndex >= 0)
        {
            if (m_listeningIndex >= static_cast<int>(m_actions.size()))
            {
                m_listeningIndex = -1;   // the action was deleted while it was listening
            }
            else if (ImGui::IsKeyPressed(ImGuiKey_Escape, false))
            {
                m_listeningIndex = -1;
            }
            else
            {
                // Keyboard/mouse through ImGui; gamepad through Core. The order matters
                // little (a key and a button cannot be pressed in the same frame), but
                // the gamepad goes after because it is the rare case. The axes go
                // last: when pushing a stick it is easy to also brush its L3/R3.
                int key = pollFirstPressedKey();
                if (key < 0) key = pollFirstPressedPadButton();
                if (key < 0) key = pollFirstPressedPadAxis();
                if (key >= 0)
                {
                    Action& a = m_actions[m_listeningIndex];
                    // A repeated binding in the same action adds nothing and would
                    // duplicate the row.
                    bool already = false;
                    for (int b : a.bindings) if (b == key) { already = true; break; }
                    if (!already) a.bindings.push_back(key);
                    m_listeningIndex = -1;
                    save();
                }
            }
        }

        // Indices of the action to delete and of the binding to remove. Deferred: the
        // vector cannot be mutated while it is being walked for drawing.
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
                        m_warning = "The name cannot be empty.";
                    }
                    else if (clash >= 0 && clash != i)
                    {
                        m_warning = "There is already an action named '" + name + "'.";
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
                    ImGui::TextDisabled("Press a key, mouse button, gamepad button, stick or trigger (Esc cancels)");
                    // Without a recognized gamepad no button arrives: saying so here
                    // avoids it looking like the listening is broken.
                    if (!glfwJoystickIsGamepad(GLFW_JOYSTICK_1))
                    {
                        ImGui::SameLine();
                        ImGui::TextDisabled("[no gamepad]");
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
                ImGui::TextDisabled("(no bindings)");
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
            // The deferred indices point to positions that have just moved.
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
