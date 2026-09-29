#pragma once
#include <GLFW/glfw3.h>

namespace DonTopo {

// What a key event means to the editor window as a whole (not to a panel).
enum class EditorKeyAction { None, CloseWindow, FocusSelected };

// The single decision for the editor-wide shortcuts. Both render paths of
// sandbox/src/main.cpp (Vulkan and DirectX 12) wire their GLFW key callback to
// this, so the two cannot drift apart again: this used to live only in the
// Vulkan callback, and under DirectX 12 neither F nor Esc did anything.
//
// wantTextInput is ImGui::GetIO().WantTextInput: while the user is typing in a
// text field, F is a letter, not "focus the selection". Esc keeps closing the
// window regardless, as it always did in the Vulkan path.
inline EditorKeyAction editorKeyAction(int key, int action, bool wantTextInput)
{
    if (action != GLFW_PRESS) return EditorKeyAction::None;
    if (key == GLFW_KEY_ESCAPE) return EditorKeyAction::CloseWindow;
    if (key == GLFW_KEY_F && !wantTextInput) return EditorKeyAction::FocusSelected;
    return EditorKeyAction::None;
}

} // namespace DonTopo
