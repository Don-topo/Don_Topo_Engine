#pragma once

// Glue between the game input (keyboard and gamepad, that is GLFW) and what
// UiCanvas understands.
//
// It lives OUTSIDE UiCanvas on purpose: the canvas is pure, deterministic CPU and
// knows neither GLFW nor Input, which is exactly what allows testing it without a window. What
// is missing without this is everything that is not the mouse: focus, Tab,
// the arrows and the gamepad's accept button were implemented and tested
// but nobody fed them, so a gamepad game could not even move through
// a menu.

#include "DonTopo/UI/UiCanvas.h"

namespace DonTopo
{
    // Fills keys (EDGES of this frame, not held keys) and the three
    // modifiers. It touches neither the mouse nor the time: that is up to the caller,
    // who is the only one that knows what space its cursor is in.
    //
    // Keyboard: Tab, Enter (also the numpad one), Escape and arrows.
    // Gamepad: the d-pad and the left stick move, A accepts and B cancels, the
    // same layout anyone who has used a console menu expects.
    void fillUiInputKeys(UiInputState& out);

    // A typed character, as a Unicode codepoint. It is called by GLFW's
    // character callback (glfwSetCharCallback) and fillUiInputKeys DRAINS it on the
    // next frame.
    //
    // An accumulator is needed because GLFW gives characters ONLY through a callback:
    // there is no "what was just typed" to query, same as with the
    // mouse wheel. Reading it without draining it would repeat the text forever.
    //
    // The caller decides WHEN to push: in the editor only during Play and with
    // ImGui without text focus, or typing in an inspector field would end up
    // inside the game's InputField too.
    void pushUiInputChar(uint32_t codepoint);

    // Discards what has accumulated without delivering it. For the frame in which the caller will NOT
    // consume the text: without this, what was typed while the game UI
    // was off would come out all at once when it is turned on.
    void discardUiInputChars();
}
