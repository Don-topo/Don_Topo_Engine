#include "DonTopo/Core/Input.h"
#include <GLFW/glfw3.h>
#include <nlohmann/json.hpp>

#include <fstream>
#include <utility>

namespace DonTopo
{
    namespace
    {
        // The same path InputActionsPanel uses (working directory).
        const char* kInputActionsFile = "input_actions.json";

        // Activation threshold of an axis and release threshold. Separate on
        // purpose: with only one, a stick left right at the limit toggles
        // active/inactive and fires an IsActionPressed per frame.
        constexpr float kAxisPressThreshold   = 0.5f;
        constexpr float kAxisReleaseThreshold = 0.4f;

        bool isTriggerAxis(int axis)
        {
            return axis == GLFW_GAMEPAD_AXIS_LEFT_TRIGGER
                || axis == GLFW_GAMEPAD_AXIS_RIGHT_TRIGGER;
        }
    }

    GLFWwindow* Input::s_window = nullptr;
    std::array<bool, 349> Input::s_curr{};
    std::array<bool, 349> Input::s_prev{};
    std::array<bool, 8>   Input::s_mCurr{};
    std::array<bool, 8>   Input::s_mPrev{};
    std::array<bool, 15>  Input::s_padCurr{};
    std::array<bool, 15>  Input::s_padPrev{};
    std::array<bool, Input::kPadAxisBindingCount> Input::s_axisCurr{};
    std::array<bool, Input::kPadAxisBindingCount> Input::s_axisPrev{};

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

        // First connected gamepad with a known mapping. With no gamepad, everything is false:
        // disconnecting it mid-game releases the buttons, it does not leave them
        // stuck (and the next frame gives the rising edge).
        s_padPrev  = s_padCurr;
        s_axisPrev = s_axisCurr;
        GLFWgamepadstate pad{};
        if (glfwJoystickIsGamepad(GLFW_JOYSTICK_1) && glfwGetGamepadState(GLFW_JOYSTICK_1, &pad))
        {
            for (int b = 0; b <= GLFW_GAMEPAD_BUTTON_LAST; ++b)
                s_padCurr[b] = pad.buttons[b] == GLFW_PRESS;
            // The hysteresis looks at the previous frame state, hence s_axisPrev
            // (s_axisCurr is already being overwritten in this same loop).
            for (int c = 0; c < kPadAxisBindingCount; ++c)
                s_axisCurr[c] = padAxisActive(c, pad.axes[padAxisIndex(c)], s_axisPrev[c]);
        }
        else
        {
            s_padCurr.fill(false);
            s_axisCurr.fill(false);
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
        // Bounded and against the frame snapshot, exactly like
        // isKeyDown above. It used to ask GLFW live and unbounded,
        // with two consequences: an invalid code caused a GLFW error
        // per query and per frame, and the value might not match the
        // one seen by isActionPressed/isActionReleased, which do read s_mCurr/s_mPrev.
        // Down, Pressed and Released now look at the same snapshot.
        return button >= 0 && button <= GLFW_MOUSE_BUTTON_LAST && s_mCurr[button];
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

    bool Input::padAxisActive(int code, float rawValue, bool wasActive)
    {
        if (code < 0 || code >= kPadAxisBindingCount) return false;

        const int  axis     = padAxisIndex(code);
        const bool negative = padAxisNegative(code);

        float value = rawValue;
        if (isTriggerAxis(axis))
        {
            // A trigger is only pressed toward one side: the negative direction
            // is not bindable and can never activate.
            if (negative) return false;
            value = (rawValue + 1.0f) * 0.5f;   // [-1,1] with rest at -1 => [0,1]
        }
        else if (negative)
        {
            value = -value;   // the binding looks at its side of the axis
        }

        return value > (wasActive ? kAxisReleaseThreshold : kAxisPressThreshold);
    }

    bool Input::isPadAxisDown(int code)
    {
        return code >= 0 && code < kPadAxisBindingCount && s_axisCurr[code];
    }
    bool Input::isPadAxisPressed(int code)
    {
        return code >= 0 && code < kPadAxisBindingCount
            && s_axisCurr[code] && !s_axisPrev[code];
    }

    void Input::ensureActionsLoaded()
    {
        if (s_actionsLoaded) return;
        s_actionsLoaded = true;   // before reading: an unreadable file is not retried every frame
        loadActionsFromDisk();
    }

    void Input::loadActionsFromDisk()
    {
        // The warnings are those of THIS load: they are cleared on entry, never during.
        // Without this, saving the panel twice with a bad binding would duplicate them
        // (same criterion as Scene::m_warnings).
        s_actionDiagnostics.clear();

        std::ifstream file(kInputActionsFile);
        if (!file.is_open()) return;   // no actions file: empty map, not an error

        // A broken JSON leaves the map empty; an exception never rises to the script.
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

            // Only "glfw": "bindings" are ImGuiKey values that Core cannot
            // translate. An action without "glfw" exists but never fires.
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

                    // Code out of range (file from another version, hand
                    // edit): it is discarded HERE and not on every query. And it is NAMED:
                    // an action that never fires because its binding was
                    // discarded at load is indistinguishable from a badly
                    // configured one if this is swallowed silently.
                    //
                    // The rule was written —with this same comment— but
                    // applied ONLY to padaxis, so the key, mouse
                    // and pad codes came in unchecked. The mouse one was the most
                    // noticeable: isActionDown resolved it with isMouseButtonDown, which
                    // passed it to GLFW unbounded (one GLFW error per
                    // query and per frame), while isActionPressed and
                    // isActionReleased did bound it — three functions of the
                    // same family behaving differently on the same file.
                    //
                    // Validating in the loader and not in the three queries is what
                    // makes the fourth query someone writes inherit the
                    // guard without having to remember.
                    const auto descarta = [&](const char* queDispositivo) {
                        s_actionDiagnostics.push_back(
                            "Input: action '" + name + "' has a binding of " +
                            queDispositivo + " with a code out of range (" +
                            std::to_string(b.code) + "); that binding is discarded");
                    };

                    if (device == "key")
                    {
                        if (b.code < 0 || b.code > GLFW_KEY_LAST) { descarta("key"); continue; }
                        b.device = ActionDevice::Key;
                    }
                    else if (device == "mouse")
                    {
                        if (b.code < 0 || b.code > GLFW_MOUSE_BUTTON_LAST)
                        {
                            descarta("mouse button");
                            continue;
                        }
                        b.device = ActionDevice::Mouse;
                    }
                    else if (device == "pad")
                    {
                        if (b.code < 0 || b.code > GLFW_GAMEPAD_BUTTON_LAST)
                        {
                            descarta("gamepad button");
                            continue;
                        }
                        b.device = ActionDevice::Pad;
                    }
                    else if (device == "padaxis")
                    {
                        if (b.code < 0 || b.code >= kPadAxisBindingCount)
                        {
                            descarta("gamepad axis");
                            continue;
                        }
                        b.device = ActionDevice::PadAxis;
                    }
                    else
                    {
                        s_actionDiagnostics.push_back(
                            "Input: action '" + name + "' has a binding of an unknown "
                            "device ('" + device + "'); that binding is discarded");
                        continue;
                    }
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
            if (b.device == ActionDevice::PadAxis && isPadAxisDown(b.code)) return true;
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
            if (b.device == ActionDevice::PadAxis && isPadAxisPressed(b.code)) return true;
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
            if (b.device == ActionDevice::PadAxis && b.code >= 0 && b.code < kPadAxisBindingCount
                && !s_axisCurr[b.code] && s_axisPrev[b.code]) return true;
        }
        return false;
    }
}
