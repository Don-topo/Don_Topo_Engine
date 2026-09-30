// Headless test of the binding translation of the Input Actions panel (no GUI
// and no gamepad connected). Plain main + asserts, no framework, same pattern as
// content_browser_tests.cpp.
//
// It covers the path that the panel could NOT walk: a gamepad button arrives as a
// GLFW code (Core reads it with glfwGetGamepadState), not as an ImGuiKey, so the
// inverse translation is needed to be able to save it as a binding.
#include "DonTopo/Core/Input.h"
#include "DonTopo/Editor/EditorShortcuts.h"
#include "DonTopo/Editor/InputActionsPanel.h"

#include <GLFW/glfw3.h>
#include <imgui.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <vector>

using namespace DonTopo;

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL: %s (line %d)\n", #cond, __LINE__); ++g_failures; } } while (0)

// Each digital gamepad button gives a distinct ImGuiKey, and that ImGuiKey goes back to the
// SAME GLFW code: if the round trip does not match, the binding is drawn with
// one name and the runtime fires with another button.
//
// Exception: GLFW_GAMEPAD_BUTTON_GUIDE (the central Xbox/PS button) has no
// ImGuiKey, so it cannot be bound and its outbound trip returns -1.
static void testPadRoundTrip()
{
    std::set<int> seen;
    for (int b = 0; b <= GLFW_GAMEPAD_BUTTON_LAST; ++b)
    {
        const int key = InputActionsPanel::padButtonToBinding(b);
        if (b == GLFW_GAMEPAD_BUTTON_GUIDE) { CHECK(key < 0); continue; }
        CHECK(key > 0);
        if (key <= 0) continue;

        // Within the range that load() accepts: a binding outside it would be
        // discarded when the file is re-read.
        CHECK(key >= ImGuiKey_NamedKey_BEGIN && key < ImGuiKey_NamedKey_END);
        // And within the gamepad block: if it fell in the keyboard one, GetKeyName
        // would draw an arbitrary key.
        CHECK(key >= ImGuiKey_GamepadStart && key <= ImGuiKey_GamepadRStickDown);
        CHECK(seen.insert(key).second);   // two buttons cannot map to the same one

        const char* device = nullptr;
        int code = -1;
        CHECK(InputActionsPanel::bindingToGlfw(key, device, code));
        CHECK(device != nullptr && std::strcmp(device, "pad") == 0);
        CHECK(code == b);                 // round trip to the same button
    }
    CHECK(seen.size() == static_cast<size_t>(GLFW_GAMEPAD_BUTTON_LAST));   // all except GUIDE
}

// An index that is not a gamepad button cannot slip through as a binding: without
// this, a GLFW_GAMEPAD_BUTTON_LAST+1 would return garbage from the ImGui enum.
static void testPadOutOfRange()
{
    CHECK(InputActionsPanel::padButtonToBinding(-1) < 0);
    CHECK(InputActionsPanel::padButtonToBinding(GLFW_GAMEPAD_BUTTON_LAST + 1) < 0);
    CHECK(InputActionsPanel::padButtonToBinding(1000) < 0);
}

// The three devices share the ImGuiKey range; the translation has to
// separate them properly, not just the gamepad.
static void testKeyboardAndMouseStillTranslate()
{
    const char* device = nullptr;
    int code = -1;

    CHECK(InputActionsPanel::bindingToGlfw(ImGuiKey_A, device, code));
    CHECK(std::strcmp(device, "key") == 0 && code == GLFW_KEY_A);

    CHECK(InputActionsPanel::bindingToGlfw(ImGuiKey_Space, device, code));
    CHECK(std::strcmp(device, "key") == 0 && code == GLFW_KEY_SPACE);

    CHECK(InputActionsPanel::bindingToGlfw(ImGuiKey_MouseLeft, device, code));
    CHECK(std::strcmp(device, "mouse") == 0 && code == GLFW_MOUSE_BUTTON_LEFT);

    CHECK(InputActionsPanel::bindingToGlfw(ImGuiKey_MouseRight, device, code));
    CHECK(std::strcmp(device, "mouse") == 0 && code == GLFW_MOUSE_BUTTON_RIGHT);

    // The mouse wheels still have no equivalent: they are ImGui axes that Core
    // does not read, and letting them through would give a binding that never fires.
    CHECK(!InputActionsPanel::bindingToGlfw(ImGuiKey_MouseWheelX, device, code));
    CHECK(!InputActionsPanel::bindingToGlfw(ImGuiKey_MouseWheelY, device, code));
}

// Triggers and sticks are axes, not buttons: they go through the "padaxis" device with
// the code axis*2+sign. Each direction is checked separately because the
// sign of GLFW's Y axis is inverted relative to what the name says
// (up is NEGATIVE), and a flipped sign sends the action to the opposite side
// without any GUI test noticing.
static void testPadAxisDirections()
{
    struct Case { int imguiKey; int axis; bool negative; };
    const Case cases[] = {
        { ImGuiKey_GamepadLStickRight, GLFW_GAMEPAD_AXIS_LEFT_X,       false },
        { ImGuiKey_GamepadLStickLeft,  GLFW_GAMEPAD_AXIS_LEFT_X,       true  },
        { ImGuiKey_GamepadLStickDown,  GLFW_GAMEPAD_AXIS_LEFT_Y,       false },
        { ImGuiKey_GamepadLStickUp,    GLFW_GAMEPAD_AXIS_LEFT_Y,       true  },
        { ImGuiKey_GamepadRStickRight, GLFW_GAMEPAD_AXIS_RIGHT_X,      false },
        { ImGuiKey_GamepadRStickLeft,  GLFW_GAMEPAD_AXIS_RIGHT_X,      true  },
        { ImGuiKey_GamepadRStickDown,  GLFW_GAMEPAD_AXIS_RIGHT_Y,      false },
        { ImGuiKey_GamepadRStickUp,    GLFW_GAMEPAD_AXIS_RIGHT_Y,      true  },
        { ImGuiKey_GamepadL2,          GLFW_GAMEPAD_AXIS_LEFT_TRIGGER, false },
        { ImGuiKey_GamepadR2,          GLFW_GAMEPAD_AXIS_RIGHT_TRIGGER,false },
    };

    for (const Case& c : cases)
    {
        const char* device = nullptr;
        int code = -1;
        CHECK(InputActionsPanel::bindingToGlfw(c.imguiKey, device, code));
        CHECK(device != nullptr && std::strcmp(device, "padaxis") == 0);
        CHECK(code == Input::padAxisCode(c.axis, c.negative));
        CHECK(Input::padAxisIndex(code) == c.axis);
        CHECK(Input::padAxisNegative(code) == c.negative);
        // And the way back: the panel captures by axis code and stores an ImGuiKey.
        CHECK(InputActionsPanel::padAxisToBinding(code) == c.imguiKey);
    }
}

// Every valid axis code goes and comes back to the same place, and no ImGuiKey is
// repeated. The two codes with no equivalent (trigger in the negative: a trigger is only
// pressed toward one side) cannot be bound.
static void testPadAxisRoundTrip()
{
    std::set<int> seen;
    for (int c = 0; c < Input::kPadAxisBindingCount; ++c)
    {
        const int key = InputActionsPanel::padAxisToBinding(c);
        const bool isTriggerNegative =
            (Input::padAxisIndex(c) == GLFW_GAMEPAD_AXIS_LEFT_TRIGGER
             || Input::padAxisIndex(c) == GLFW_GAMEPAD_AXIS_RIGHT_TRIGGER)
            && Input::padAxisNegative(c);
        if (isTriggerNegative) { CHECK(key < 0); continue; }

        CHECK(key > 0);
        if (key <= 0) continue;
        CHECK(key >= ImGuiKey_NamedKey_BEGIN && key < ImGuiKey_NamedKey_END);
        CHECK(key >= ImGuiKey_GamepadStart && key <= ImGuiKey_GamepadRStickDown);
        CHECK(seen.insert(key).second);

        const char* device = nullptr;
        int code = -1;
        CHECK(InputActionsPanel::bindingToGlfw(key, device, code));
        CHECK(device != nullptr && std::strcmp(device, "padaxis") == 0);
        CHECK(code == c);
    }
    CHECK(seen.size() == 10);   // 8 stick directions + 2 triggers
}

static void testPadAxisOutOfRange()
{
    CHECK(InputActionsPanel::padAxisToBinding(-1) < 0);
    CHECK(InputActionsPanel::padAxisToBinding(Input::kPadAxisBindingCount) < 0);
    CHECK(InputActionsPanel::padAxisToBinding(1000) < 0);
}

// The digitization of the axis: threshold with hysteresis (activates at 0.5, releases at 0.4)
// so that a stick resting right at the edge does not spit out an edge per frame.
static void testPadAxisHysteresis()
{
    const int right = Input::padAxisCode(GLFW_GAMEPAD_AXIS_LEFT_X, false);
    CHECK(!Input::padAxisActive(right, 0.30f, false));   // below the threshold
    CHECK(Input::padAxisActive(right, 0.60f, false));    // crosses it
    CHECK(Input::padAxisActive(right, 0.45f, true));     // dead zone: stays active
    CHECK(!Input::padAxisActive(right, 0.35f, true));    // falls below the release one
    // Pushing to the opposite side does not activate this direction.
    CHECK(!Input::padAxisActive(right, -0.90f, false));

    const int left = Input::padAxisCode(GLFW_GAMEPAD_AXIS_LEFT_X, true);
    CHECK(Input::padAxisActive(left, -0.60f, false));
    CHECK(!Input::padAxisActive(left, 0.60f, false));
}

// GLFW gives the triggers in [-1, 1] with rest at -1, not at 0: without
// renormalizing to [0, 1] the trigger would have to be squeezed to 75% to reach the
// threshold, and at rest the value would be a hair away from activating.
static void testPadTriggerRange()
{
    const int l2 = Input::padAxisCode(GLFW_GAMEPAD_AXIS_LEFT_TRIGGER, false);
    CHECK(!Input::padAxisActive(l2, -1.00f, false));   // rest
    CHECK(!Input::padAxisActive(l2, -1.00f, true));    // and releases if it was pressed
    CHECK(!Input::padAxisActive(l2, -0.20f, false));   // 40% of travel
    CHECK(Input::padAxisActive(l2, 0.20f, false));     // 60%: crosses the threshold
    CHECK(Input::padAxisActive(l2, 1.00f, false));     // fully pressed

    // The trigger in the negative does not exist as a binding and can never activate.
    const int l2neg = Input::padAxisCode(GLFW_GAMEPAD_AXIS_LEFT_TRIGGER, true);
    CHECK(!Input::padAxisActive(l2neg, -1.00f, false));
    CHECK(!Input::padAxisActive(l2neg, 1.00f, false));
}

// With no gamepad connected, Input::update does not touch the axes and every query gives false:
// an action bound to a stick cannot fire by itself on a PC without a gamepad.
static void testPadAxisWithoutGamepad()
{
    for (int c = 0; c < Input::kPadAxisBindingCount; ++c)
    {
        CHECK(!Input::isPadAxisDown(c));
        CHECK(!Input::isPadAxisPressed(c));
    }
    CHECK(!Input::isPadAxisDown(-1));
    CHECK(!Input::isPadAxisDown(Input::kPadAxisBindingCount));
}

// H5 of docs/core-audit.md: takeActionDiagnostics() ALWAYS returned an empty
// list. The channel exists (the header promises it and ScriptBindings.cpp:501 dumps it
// to the Log) but nobody ever wrote into it: the two places that discard
// a binding on load (axis code out of range and unknown device)
// did so with a silent `continue`.
//
// What it cost: an action that never fires because its binding was discarded
// on load is indistinguishable from a misconfigured action.
//
// The file lives in the working directory and is THE SAME one the editor uses,
// so the test backs up the user's and restores it when it finishes.
static void testActionDiagnosticsReportDiscardedBindings()
{
    const char* kFile = "input_actions.json";
    std::string previo;
    bool habia = false;
    {
        std::ifstream in(kFile, std::ios::binary);
        if (in)
        {
            habia = true;
            previo.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }
    }

    {
        std::ofstream out(kFile);
        out << "{\"actions\":[{\"name\":\"Saltar\",\"glfw\":["
               "{\"device\":\"volante\",\"code\":3},"
               "{\"device\":\"padaxis\",\"code\":999},"
               "{\"device\":\"key\",\"code\":32}]}]}";
    }

    Input::reloadActions();
    std::vector<std::string> avisos = Input::takeActionDiagnostics();

    // Two discards: the device that does not exist and the axis out of range.
    CHECK(avisos.size() == 2);
    int nombran = 0;
    for (const std::string& a : avisos)
        if (a.find("Saltar") != std::string::npos) ++nombran;
    CHECK(nombran == 2);

    // It is emptied when read: the consumer dumps them to the Log only once.
    CHECK(Input::takeActionDiagnostics().empty());

    // And the action still exists with its good binding: discarding a bad one does not
    // take the others with it.
    CHECK(Input::hasAction("Saltar"));
    CHECK(Input::isActionDown("Saltar") == false);   // no window, no keys

    if (habia)
    {
        std::ofstream out(kFile, std::ios::binary);
        out << previo;
    }
    else
    {
        std::remove(kFile);
    }
}

// H10 of docs/core-audit.md, extended by what was seen when looking at it: the loader
// already has the rule written in its own comment ("code out of range: it is
// discarded HERE and not on every query, and it is NAMED") but only applied it to
// padaxis. The key, mouse and pad codes came in without being looked at.
//
// What it cost, per device:
//   - mouse: isActionDown resolved with isMouseButtonDown, which passed the
//     code to GLFW unbounded (a GLFW error per query and per frame),
//     while isActionPressed/Released discarded it. Three functions of the
//     same family, three behaviors for the SAME file.
//   - key and pad: the binding stayed alive and mute, and an action that never fires
//     is indistinguishable from a misconfigured one.
//
// It is tested in the loader and not in the queries on purpose: without a GLFW
// window every query returns false no matter what, so a test there would
// pass just as broken (it is the case of the fixture that never reaches the line).
static void testOutOfRangeCodesDiscardedForEveryDevice()
{
    const char* kFile = "input_actions.json";
    std::string previo;
    bool habia = false;
    {
        std::ifstream in(kFile, std::ios::binary);
        if (in)
        {
            habia = true;
            previo.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }
    }

    {
        std::ofstream out(kFile);
        out << "{\"actions\":[{\"name\":\"Disparar\",\"glfw\":["
               "{\"device\":\"mouse\",\"code\":99},"      // out of range
               "{\"device\":\"mouse\",\"code\":-1},"      // negative
               "{\"device\":\"key\",\"code\":50000},"     // out of range
               "{\"device\":\"pad\",\"code\":77},"        // out of range
               "{\"device\":\"mouse\",\"code\":0}]}]}";   // the good one, it stays
    }

    Input::reloadActions();
    std::vector<std::string> avisos = Input::takeActionDiagnostics();

    // Four discards, one per bad binding, and all of them NAME the action: without the
    // name the warning is useless for fixing the file.
    CHECK(avisos.size() == 4);
    int nombran = 0;
    for (const std::string& a : avisos)
        if (a.find("Disparar") != std::string::npos) ++nombran;
    CHECK(nombran == 4);

    // And the good binding survives: discarding the bad ones does not take the action.
    CHECK(Input::hasAction("Disparar"));

    if (habia)
    {
        std::ofstream out(kFile, std::ios::binary);
        out << previo;
    }
    else
    {
        std::remove(kFile);
    }
}

// Editor window shortcuts, shared by the Vulkan and the DirectX 12 paths of
// sandbox/src/main.cpp. Before this lived only in the Vulkan key callback, so
// under DirectX 12 neither F (focus) nor Esc (close) did anything.
static void testEditorShortcuts()
{
    using DonTopo::EditorKeyAction;
    using DonTopo::editorKeyAction;
    CHECK(editorKeyAction(GLFW_KEY_F, GLFW_PRESS, false) == EditorKeyAction::FocusSelected);
    // Typing an "f" into a text field (Hierarchy rename, Script Editor) must not jump the camera.
    CHECK(editorKeyAction(GLFW_KEY_F, GLFW_PRESS, true) == EditorKeyAction::None);
    CHECK(editorKeyAction(GLFW_KEY_ESCAPE, GLFW_PRESS, false) == EditorKeyAction::CloseWindow);
    // Esc closes the window even while typing: it did in the Vulkan path, and
    // that behaviour is kept as it was.
    CHECK(editorKeyAction(GLFW_KEY_ESCAPE, GLFW_PRESS, true) == EditorKeyAction::CloseWindow);
    // Only the press counts: release and key repeat do nothing.
    CHECK(editorKeyAction(GLFW_KEY_F, GLFW_RELEASE, false) == EditorKeyAction::None);
    CHECK(editorKeyAction(GLFW_KEY_F, GLFW_REPEAT, false) == EditorKeyAction::None);
    CHECK(editorKeyAction(GLFW_KEY_ESCAPE, GLFW_RELEASE, false) == EditorKeyAction::None);
    CHECK(editorKeyAction(GLFW_KEY_G, GLFW_PRESS, false) == EditorKeyAction::None);
}

int main()
{
    testEditorShortcuts();
    testPadRoundTrip();
    testPadOutOfRange();
    testKeyboardAndMouseStillTranslate();
    testPadAxisDirections();
    testPadAxisRoundTrip();
    testPadAxisOutOfRange();
    testPadAxisHysteresis();
    testPadTriggerRange();
    testPadAxisWithoutGamepad();
    testActionDiagnosticsReportDiscardedBindings();
    testOutOfRangeCodesDiscardedForEveryDevice();

    if (g_failures == 0) std::printf("input_actions_tests: OK\n");
    else                 std::printf("input_actions_tests: %d FAILURES\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
