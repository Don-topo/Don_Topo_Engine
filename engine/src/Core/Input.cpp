#include "DonTopo/Core/Input.h"
#include <GLFW/glfw3.h>
#include <nlohmann/json.hpp>

#include <fstream>
#include <utility>

namespace DonTopo
{
    namespace
    {
        // La misma ruta que usa InputActionsPanel (directorio de trabajo).
        const char* kInputActionsFile = "input_actions.json";
    }

    GLFWwindow* Input::s_window = nullptr;
    std::array<bool, 349> Input::s_curr{};
    std::array<bool, 349> Input::s_prev{};
    std::array<bool, 8>   Input::s_mCurr{};
    std::array<bool, 8>   Input::s_mPrev{};
    std::array<bool, 15>  Input::s_padCurr{};
    std::array<bool, 15>  Input::s_padPrev{};

    std::unordered_map<std::string, std::vector<ActionBinding>> Input::s_actions;
    bool Input::s_actionsLoaded = false;
    std::vector<std::string> Input::s_actionDiagnostics;

    void Input::init(GLFWwindow* window) { s_window = window; }

    void Input::update()
    {
        if (!s_window) return;
        s_prev = s_curr;
        for (int k = GLFW_KEY_SPACE; k <= GLFW_KEY_LAST; ++k)
            s_curr[k] = glfwGetKey(s_window, k) == GLFW_PRESS;

        s_mPrev = s_mCurr;
        for (int b = 0; b <= GLFW_MOUSE_BUTTON_LAST; ++b)
            s_mCurr[b] = glfwGetMouseButton(s_window, b) == GLFW_PRESS;

        // Primer mando conectado con mapeo conocido. Sin mando, todo a false:
        // desconectarlo a mitad de partida suelta los botones, no los deja
        // pegados (y el frame siguiente da el flanco de subida).
        s_padPrev = s_padCurr;
        GLFWgamepadstate pad{};
        if (glfwJoystickIsGamepad(GLFW_JOYSTICK_1) && glfwGetGamepadState(GLFW_JOYSTICK_1, &pad))
        {
            for (int b = 0; b <= GLFW_GAMEPAD_BUTTON_LAST; ++b)
                s_padCurr[b] = pad.buttons[b] == GLFW_PRESS;
        }
        else
        {
            s_padCurr.fill(false);
        }
    }

    bool Input::isKeyDown(int key)
    {
        return key >= 0 && key <= GLFW_KEY_LAST && s_curr[key];
    }
    bool Input::isKeyPressed(int key)
    {
        return key >= 0 && key <= GLFW_KEY_LAST && s_curr[key] && !s_prev[key];
    }
    bool Input::isKeyReleased(int key)
    {
        return key >= 0 && key <= GLFW_KEY_LAST && !s_curr[key] && s_prev[key];
    }
    bool Input::isMouseButtonDown(int button)
    {
        return s_window && glfwGetMouseButton(s_window, button) == GLFW_PRESS;
    }
    bool Input::isPadButtonDown(int button)
    {
        return button >= 0 && button <= GLFW_GAMEPAD_BUTTON_LAST && s_padCurr[button];
    }
    bool Input::isPadButtonPressed(int button)
    {
        return button >= 0 && button <= GLFW_GAMEPAD_BUTTON_LAST
            && s_padCurr[button] && !s_padPrev[button];
    }

    void Input::ensureActionsLoaded()
    {
        if (s_actionsLoaded) return;
        s_actionsLoaded = true;   // antes de leer: un fichero ilegible no se reintenta cada frame
        loadActionsFromDisk();
    }

    void Input::loadActionsFromDisk()
    {
        std::ifstream file(kInputActionsFile);
        if (!file.is_open()) return;   // sin fichero de acciones: mapa vacío, no es un error

        // Un JSON roto deja el mapa vacío; nunca sube una excepción al script.
        nlohmann::json j = nlohmann::json::parse(file, nullptr, false);
        if (j.is_discarded() || !j.is_object()) return;

        auto actionsIt = j.find("actions");
        if (actionsIt == j.end() || !actionsIt->is_array()) return;

        for (const auto& aj : *actionsIt)
        {
            if (!aj.is_object()) continue;
            auto nameIt = aj.find("name");
            if (nameIt == aj.end() || !nameIt->is_string()) continue;
            const std::string name = nameIt->get<std::string>();
            if (name.empty()) continue;

            // Solo "glfw": "bindings" son valores de ImGuiKey que Core no sabe
            // traducir. Una acción sin "glfw" existe pero no dispara nunca.
            std::vector<ActionBinding> bindings;
            auto glfwIt = aj.find("glfw");
            if (glfwIt != aj.end() && glfwIt->is_array())
            {
                for (const auto& bj : *glfwIt)
                {
                    if (!bj.is_object()) continue;
                    auto devIt  = bj.find("device");
                    auto codeIt = bj.find("code");
                    if (devIt == bj.end() || !devIt->is_string()) continue;
                    if (codeIt == bj.end() || !codeIt->is_number_integer()) continue;

                    const std::string device = devIt->get<std::string>();
                    ActionBinding b;
                    b.code = codeIt->get<int>();
                    if (device == "key")        b.device = ActionDevice::Key;
                    else if (device == "mouse") b.device = ActionDevice::Mouse;
                    else if (device == "pad")   b.device = ActionDevice::Pad;
                    else continue;
                    bindings.push_back(b);
                }
            }

            s_actions[name] = std::move(bindings);
        }
    }

    void Input::reloadActions()
    {
        s_actions.clear();
        s_actionsLoaded = true;
        loadActionsFromDisk();
    }

    std::vector<std::string> Input::takeActionDiagnostics()
    {
        std::vector<std::string> out;
        out.swap(s_actionDiagnostics);
        return out;
    }

    bool Input::hasAction(const std::string& name)
    {
        ensureActionsLoaded();
        return s_actions.find(name) != s_actions.end();
    }

    bool Input::isActionDown(const std::string& name)
    {
        ensureActionsLoaded();
        auto it = s_actions.find(name);
        if (it == s_actions.end()) return false;
        for (const ActionBinding& b : it->second)
        {
            if (b.device == ActionDevice::Key && isKeyDown(b.code)) return true;
            if (b.device == ActionDevice::Mouse && isMouseButtonDown(b.code)) return true;
            if (b.device == ActionDevice::Pad && b.code >= 0 && b.code <= GLFW_GAMEPAD_BUTTON_LAST
                && s_padCurr[b.code]) return true;
        }
        return false;
    }

    bool Input::isActionPressed(const std::string& name)
    {
        ensureActionsLoaded();
        auto it = s_actions.find(name);
        if (it == s_actions.end()) return false;
        for (const ActionBinding& b : it->second)
        {
            if (b.device == ActionDevice::Key && isKeyPressed(b.code)) return true;
            if (b.device == ActionDevice::Mouse && b.code >= 0 && b.code <= GLFW_MOUSE_BUTTON_LAST
                && s_mCurr[b.code] && !s_mPrev[b.code]) return true;
            if (b.device == ActionDevice::Pad && b.code >= 0 && b.code <= GLFW_GAMEPAD_BUTTON_LAST
                && s_padCurr[b.code] && !s_padPrev[b.code]) return true;
        }
        return false;
    }

    bool Input::isActionReleased(const std::string& name)
    {
        ensureActionsLoaded();
        auto it = s_actions.find(name);
        if (it == s_actions.end()) return false;
        for (const ActionBinding& b : it->second)
        {
            if (b.device == ActionDevice::Key && isKeyReleased(b.code)) return true;
            if (b.device == ActionDevice::Mouse && b.code >= 0 && b.code <= GLFW_MOUSE_BUTTON_LAST
                && !s_mCurr[b.code] && s_mPrev[b.code]) return true;
            if (b.device == ActionDevice::Pad && b.code >= 0 && b.code <= GLFW_GAMEPAD_BUTTON_LAST
                && !s_padCurr[b.code] && s_padPrev[b.code]) return true;
        }
        return false;
    }
}
