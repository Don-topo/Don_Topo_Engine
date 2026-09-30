#pragma once
#include <string>
#include <vector>

namespace DonTopo {

// "Input Actions" window: map of named actions to keys, mouse
// buttons, gamepad buttons and stick/trigger directions.
//
// Its own panel and not a Properties block because the map is global to the
// project, not per GameObject: there is no selection to hang it from.
//
// Binding capture uses ImGui's key API directly
// (ImGuiKey_NamedKey_BEGIN..END, which already includes ImGuiKey_Mouse* and
// ImGuiKey_Gamepad*) instead of an input layer of its own: the panel only lives
// while the editor has focus and ImGui already has all three devices there.
class InputActionsPanel {
public:
    // Loads the persistence JSON if it exists. A missing or corrupt file
    // leaves the panel empty, it never aborts the editor startup.
    InputActionsPanel();

    void draw();
    bool* GetOpenPtr() { return &m_open; }
    void open() { m_open = true; }

    struct Action {
        std::string name;
        // ImGuiKey values. They are stored as int so as not to drag <imgui.h>
        // into everything that includes this header (same criterion as the rest of the
        // editor headers).
        std::vector<int> bindings;
    };

    // --- Translation between the panel's model (ImGuiKey) and what Core
    // understands (GLFW codes). Public and static so they can be tested headless:
    // a mismatch between the round trip draws a binding with one name and
    // fires it with another button, and no GUI test sees that.

    // ImGuiKey -> device ("key"/"mouse"/"pad"/"padaxis") and GLFW code.
    // Returns false if there is no equivalent (mouse wheels, exotic keys):
    // that binding is still drawn in the panel but does not reach the
    // runtime map.
    static bool bindingToGlfw(int imguiKey, const char*& outDevice, int& outCode);
    // GLFW_GAMEPAD_BUTTON_* -> ImGuiKey, or -1 if the index is not a button. It is
    // the inverse of bindingToGlfw for the gamepad: the panel captures the buttons
    // through GLFW (see pollFirstPressedPadButton) and stores them as ImGuiKey.
    static int padButtonToBinding(int glfwButton);
    // Axis code (Input::padAxisCode) -> ImGuiKey, or -1 if that code is not
    // bindable (trigger in the negative). Inverse of bindingToGlfw for "padaxis".
    static int padAxisToBinding(int axisCode);

private:
    bool load();   // returns false if there was no file or it was not readable
    void save() const;
    // Index of the action with that name, or -1. Exact comparison: two
    // actions that differ only in case are two distinct actions.
    int findAction(const std::string& name) const;
    // Walks the ImGuiKey range and returns the first key/button pressed
    // this frame, or -1. Esc is never returned: it is consumed by the cancellation.
    int pollFirstPressedKey() const;
    // First gamepad button pressed this frame, already translated to ImGuiKey, or -1.
    // It goes through Core (glfwGetGamepadState) and not ImGui: the GLFW backend only
    // feeds the ImGuiKey_Gamepad* if the editor enables NavEnableGamepad, and that
    // would make the gamepad navigate the whole interface (in Play Mode, the jump
    // button would also activate the focused widget).
    int pollFirstPressedPadButton() const;
    // First stick or trigger direction crossed this frame, already translated to
    // ImGuiKey, or -1. Same as the buttons one: through Core, not ImGui, which does not
    // even expose the gamepad axes as keys.
    int pollFirstPressedPadAxis() const;

    bool m_open = false;   // starts closed: it is a specialized panel

    std::vector<Action> m_actions;

    // Buffers of the creation bar and of the inline rename. ImGui needs
    // stable storage between frames, hence they are members.
    char m_newNameBuf[64]  = {};
    char m_renameBuf[64]   = {};
    // Index of the action being renamed, or -1. Index and not name because the rename
    // changes precisely the name.
    int  m_renamingIndex = -1;

    // Index of the action waiting for a binding, or -1 if there is no listening.
    int  m_listeningIndex = -1;

    // The loaded file came without the "glfw" array (older than the actions in
    // scripting): it is rewritten once, migrated, right after loading.
    bool m_needsMigrationSave = false;

    // Warning shown in the panel itself (duplicate name, empty name...).
    // It is cleared as soon as the operation that caused it is retried.
    std::string m_warning;
};

} // namespace DonTopo
