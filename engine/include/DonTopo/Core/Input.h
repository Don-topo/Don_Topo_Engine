#pragma once
#include <array>
#include <string>
#include <unordered_map>
#include <vector>

struct GLFWwindow;

namespace DonTopo {

// Device of an action binding, already translated to GLFW codes by the
// editor: Core cannot speak ImGuiKey (Core/Editor split).
//
// PadAxis are the sticks and the triggers: in GLFW they are analog axes, not
// buttons, so they carry their own device and their own code space
// (see Input::padAxisCode).
enum class ActionDevice { Key, Mouse, Pad, PadAxis };

struct ActionBinding {
    ActionDevice device = ActionDevice::Key;
    int          code   = -1;   // GLFW_KEY_*, GLFW_MOUSE_BUTTON_*, GLFW_GAMEPAD_BUTTON_*
                                // or axis code (PadAxis), depending on device
};

// Static facade over the GLFW keyboard/mouse with per-frame prev/curr state
// — allows IsKeyPressed/IsKeyReleased (edges), which glfwGetKey alone
// does not give. Only the scripting bindings use it for now (Camera still uses
// glfwGetKey directly — migrating it is out of scope).
class Input {
public:
    static void init(GLFWwindow* window);
    // Call once per frame, before running scripts.
    static void update();

    static bool isKeyDown(int key);      // held
    static bool isKeyPressed(int key);   // only the frame of the falling edge
    static bool isKeyReleased(int key);  // only the frame of the rising edge
    static bool isMouseButtonDown(int button);

    // --- Gamepad (first connected gamepad with a known mapping) ---
    // Index = GLFW_GAMEPAD_BUTTON_*. With no gamepad connected, always false. The
    // Input Actions panel uses them to capture bindings: ImGui does not see the gamepad
    // unless NavEnableGamepad is enabled, and that would make the gamepad navigate the
    // editor interface.
    static bool isPadButtonDown(int button);
    static bool isPadButtonPressed(int button);   // only the frame of the edge

    // --- Gamepad axes (sticks and triggers) as if they were buttons ---
    // An axis direction is a binding: "left stick up" is worth
    // the same as a key. The code packs the axis and the sign, because a
    // binding only carries an int and the two directions of one axis are two
    // different actions.
    static constexpr int kPadAxisBindingCount = 12;   // (GLFW_GAMEPAD_AXIS_LAST+1) * 2
    static constexpr int padAxisCode(int axis, bool negative)
    { return axis * 2 + (negative ? 1 : 0); }
    static constexpr int  padAxisIndex(int code)    { return code / 2; }
    static constexpr bool padAxisNegative(int code) { return (code % 2) != 0; }

    // Digitizes an axis: raw GLFW value + previous frame state => new
    // state. Threshold with hysteresis (0.5 activates, 0.4 releases) so that a
    // stick resting on the edge does not give an edge per frame. Public so it can
    // be tested without a connected gamepad. Triggers come in [-1, 1] with
    // rest at -1 and are renormalized here to [0, 1].
    static bool padAxisActive(int code, float rawValue, bool wasActive);

    static bool isPadAxisDown(int code);
    static bool isPadAxisPressed(int code);   // only the frame of the edge

    // --- Named actions (editor Input Actions panel) ---
    // An action is active if ANY of its bindings is (OR). The
    // map is loaded from input_actions.json the first time it is queried;
    // missing or corrupt file => empty map, no exception and no crash.
    // An unknown name returns false: the warning to the Log is given by whoever
    // asks (Core has no log channel), via hasAction().
    static bool isActionDown(const std::string& name);
    static bool isActionPressed(const std::string& name);
    static bool isActionReleased(const std::string& name);
    static bool hasAction(const std::string& name);
    // Rereads the file: the editor calls it when saving the panel so that a
    // newly created action works in the next Play without restarting.
    static void reloadActions();
    // Warnings of the LAST load: bindings discarded for having an axis code
    // out of range or a device that Core cannot translate. It is emptied
    // on reading —the consumer dumps them to the Log only once— and also
    // at the start of each load, so they do not accumulate between panel reloads.
    //
    // Without this, an action that never fires because its binding was discarded
    // at load is indistinguishable from a badly configured action.
    static std::vector<std::string> takeActionDiagnostics();

private:
    static void ensureActionsLoaded();
    static void loadActionsFromDisk();

    static GLFWwindow* s_window;
    // GLFW_KEY_LAST+1 entries; index = GLFW keycode.
    static std::array<bool, 349> s_curr;
    static std::array<bool, 349> s_prev;
    // GLFW_MOUSE_BUTTON_LAST+1 entries; its own prev/curr is needed because
    // isMouseButtonDown queries GLFW directly and gives no edges.
    static std::array<bool, 8> s_mCurr;
    static std::array<bool, 8> s_mPrev;
    // GLFW_GAMEPAD_BUTTON_LAST+1 entries, from the first connected gamepad.
    static std::array<bool, 15> s_padCurr;
    static std::array<bool, 15> s_padPrev;
    // Already digitized axes; index = padAxisCode() code.
    static std::array<bool, kPadAxisBindingCount> s_axisCurr;
    static std::array<bool, kPadAxisBindingCount> s_axisPrev;

    static std::unordered_map<std::string, std::vector<ActionBinding>> s_actions;
    static bool s_actionsLoaded;
    static std::vector<std::string> s_actionDiagnostics;
};

} // namespace DonTopo
